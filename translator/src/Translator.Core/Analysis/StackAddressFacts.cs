using System;
using System.Collections.Generic;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

/// <summary>
/// Simple, intraprocedural stack-address facts for local codegen and stack helper selection. Scans the
/// IR in block order following register copies and register-plus-constant add/sub, treating every SSA
/// spelling of r1 as the stack root. No CFG joins or phi analysis; that's the entry-frame/affine passes' job.
/// </summary>
internal sealed class StackAddressFacts
{
    private readonly Dictionary<string, int> _temporaryOffsets;
    private readonly bool _smallDataBasesIntact;

    private StackAddressFacts(Dictionary<string, int> temporaryOffsets, bool smallDataBasesIntact)
    {
        _temporaryOffsets = temporaryOffsets;
        _smallDataBasesIntact = smallDataBasesIntact;
    }

    public static StackAddressFacts Empty { get; } =
        new(new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase), smallDataBasesIntact: false);

    /// <summary>
    /// True for a d-form access off r13 (_SDA_BASE_) or r2 (_SDA2_BASE_) in a function that never writes
    /// either register. The EABI sets both once at startup to the middle of a 64 KiB window inside the DOL's
    /// data, so base + signed 16-bit offset is ordinary MEM1 and never the MMIO window: the store needs no
    /// MMIO policy test. Not a stack fact, so it never feeds the stack-slot passes.
    /// </summary>
    public bool IsSmallDataAddress(IrAddress address)
    {
        if (!_smallDataBasesIntact || address.Offset < short.MinValue || address.Offset > short.MaxValue)
        {
            return false;
        }

        var baseName = GetRegisterBaseName(address.Base);
        return baseName.Equals("r13", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("r2", StringComparison.OrdinalIgnoreCase);
    }

    public static StackAddressFacts Build(IrFunction function)
    {
        var temporaryOffsets = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        var smallDataBasesIntact = true;
        foreach (var block in function.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
                {
                    var name = GetRegisterBaseName(definition);
                    if (name.Equals("r13", StringComparison.OrdinalIgnoreCase) ||
                        name.Equals("r2", StringComparison.OrdinalIgnoreCase))
                    {
                        smallDataBasesIntact = false;
                    }
                }

                switch (instruction)
                {
                    // SSA form makes an architectural register destination as stable as a compiler
                    // temporary (e.g. PPC prologues copying r1 to r11 for the save area).
                    case IrAssign assign when
                        TryResolveValue(assign.Value, temporaryOffsets, out var assignOffset):
                        temporaryOffsets[assign.Destination] = assignOffset;
                        break;

                    case IrBinary binary when
                        TryResolveBinary(binary, temporaryOffsets, out var binaryOffset):
                        temporaryOffsets[binary.Destination] = binaryOffset;
                        break;
                }
            }
        }

        return new StackAddressFacts(temporaryOffsets, smallDataBasesIntact);
    }

    public bool TryResolve(IrAddress address, out int offset)
    {
        if (TryResolveRegister(address.Base, out var baseOffset))
        {
            offset = checked(baseOffset + address.Offset);
            return true;
        }

        offset = 0;
        return false;
    }

    public bool TryResolve(IrValue value, out int offset)
    {
        if (value.Kind == "register" &&
            TryResolveRegister(value.RegisterName, out offset))
        {
            return true;
        }

        offset = 0;
        return false;
    }

    public bool TryResolve(IrBinary binary, out int offset) =>
        TryResolveBinary(binary, _temporaryOffsets, out offset);

    public bool TryResolveRegister(string? registerName, out int offset)
    {
        if (string.IsNullOrWhiteSpace(registerName))
        {
            offset = 0;
            return false;
        }

        if (GetRegisterBaseName(registerName).Equals("r1", StringComparison.OrdinalIgnoreCase))
        {
            offset = 0;
            return true;
        }

        return _temporaryOffsets.TryGetValue(registerName, out offset);
    }

    public bool ContainsTemporary(string registerName) =>
        _temporaryOffsets.ContainsKey(registerName);

    private static bool TryResolveBinary(
        IrBinary binary,
        IReadOnlyDictionary<string, int> temporaryOffsets,
        out int offset) =>
        TryResolveValue(binary.Left, temporaryOffsets, out offset) &&
        TryGetIntConstant(binary.Right, out var constant) &&
        TryApplyOffset(binary.Op, offset, constant, out offset);

    private static bool TryResolveValue(
        IrValue value,
        IReadOnlyDictionary<string, int> temporaryOffsets,
        out int offset)
    {
        if (value.Kind == "register" &&
            TryResolveRegister(value.RegisterName, temporaryOffsets, out offset))
        {
            return true;
        }

        offset = 0;
        return false;
    }

    private static bool TryResolveRegister(
        string? registerName,
        IReadOnlyDictionary<string, int> temporaryOffsets,
        out int offset)
    {
        if (string.IsNullOrWhiteSpace(registerName))
        {
            offset = 0;
            return false;
        }

        if (GetRegisterBaseName(registerName).Equals("r1", StringComparison.OrdinalIgnoreCase))
        {
            offset = 0;
            return true;
        }

        return temporaryOffsets.TryGetValue(registerName, out offset);
    }

    private static bool TryGetIntConstant(IrValue value, out int result)
    {
        if (value.Kind == "const" &&
            value.Constant is { } constant &&
            constant >= int.MinValue &&
            constant <= int.MaxValue)
        {
            result = (int)constant;
            return true;
        }

        result = 0;
        return false;
    }

    private static bool TryApplyOffset(string op, int baseOffset, int constant, out int offset)
    {
        offset = 0;
        switch (op)
        {
            case "add":
                offset = checked(baseOffset + constant);
                return true;
            case "sub":
                offset = checked(baseOffset - constant);
                return true;
            default:
                return false;
        }
    }

    private static string GetRegisterBaseName(string name)
    {
        var underscore = name.IndexOf('_');
        if (underscore < 0 || underscore + 1 >= name.Length)
        {
            return name;
        }

        for (var index = underscore + 1; index < name.Length; index++)
        {
            if (!char.IsDigit(name[index]))
            {
                return name;
            }
        }

        return name[..underscore];
    }
}
