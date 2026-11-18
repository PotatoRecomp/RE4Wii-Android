using System;
using System.Collections.Generic;
using Translator.Core.Ir;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    private const uint MagicIntToDoubleHigh = 0x43300000u;
    private const ulong MagicSignedBias = 0x4330000080000000ul;
    private const ulong MagicUnsignedBias = 0x4330000000000000ul;

    /// <summary>
    /// Finishes the int-to-double idiom that <see cref="ForwardStackWordPairLoads"/> starts.
    /// After forwarding, the double is <c>fpr_from_words(0x43300000, lo)</c>, which is exactly
    /// 2^52 + lo, and the compiler then subtracts a constant from .sdata2:
    /// 2^52 + 2^31 (signed, lo = x ^ 0x80000000) or 2^52 (unsigned). Both subtractions are exact,
    /// so <c>fsub</c> becomes <c>(double)(int32)(lo ^ 0x80000000)</c> or <c>(double)lo</c>
    /// (one scvtf/ucvtf), and <c>fsubs</c> the same followed by <c>frsp</c>, which is what
    /// <c>PPC_Fsubs</c> does to an exact difference. The bias load and the bit-cast stay in the
    /// IR and die in the host compiler once nothing reads them.
    ///
    /// The bias is read from the DOL image at r2 + offset, so the fold only fires when the
    /// function never writes r2 (it is _SDA2_BASE_ for the whole program) and the image holds one
    /// of the two exact bias values there. Everything is tracked inside one block; a write to any
    /// tracked name, or any instruction this pass does not model, forgets what it depends on.
    /// </summary>
    private static IrFunction FoldMagicIntToDouble(IrFunction func, Func<int, ulong?>? sda2DoubleAt)
    {
        if (sda2DoubleAt is null || WritesRegister(func, "r2"))
        {
            return func;
        }

        var changed = false;
        var rewrittenBlocks = new List<IrBasicBlock>(func.Blocks.Count);
        foreach (var block in func.Blocks)
        {
            var instructions = block.Instructions;
            var known = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);
            var bits = new Dictionary<string, IrValue>(StringComparer.OrdinalIgnoreCase);
            var biases = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase); // name -> signed
            Dictionary<int, IrInstruction[]>? replacements = null;

            for (var index = 0; index < instructions.Count; index++)
            {
                var instruction = instructions[index];
                IrValue? low = null;
                var signed = false;
                string? destination = null;
                var single = false;

                switch (instruction)
                {
                    case IrBinary { Op: "sub" } sub when IsFloatRegister(sub.Destination) &&
                                                          TryMatch(sub.Left, sub.Right, out low, out signed):
                        destination = sub.Destination;
                        break;
                    case IrCall call when call.Target.Equals("PPC_Fsubs", StringComparison.OrdinalIgnoreCase) &&
                                          call.Arguments.Count == 2 &&
                                          !string.IsNullOrWhiteSpace(call.Destination) &&
                                          IsFloatRegister(call.Destination) &&
                                          TryMatch(call.Arguments[0], call.Arguments[1], out low, out signed):
                        destination = call.Destination;
                        single = true;
                        break;
                }

                if (destination is not null && low is not null)
                {
                    var op = signed ? "s32x_to_double" : "u32_to_double";
                    var converted = new IrBinary(destination, low, IrValue.Imm(0), op);
                    replacements ??= new Dictionary<int, IrInstruction[]>();
                    replacements[index] = single
                        ? new IrInstruction[]
                        {
                            converted,
                            new IrBinary(destination, IrValue.Register(destination), IrValue.Imm(0), "frsp"),
                        }
                        : new IrInstruction[] { converted };
                    changed = true;
                }

                Record(instruction);
            }

            if (replacements is null)
            {
                rewrittenBlocks.Add(block);
                continue;
            }

            var rewritten = new List<IrInstruction>(instructions.Count + replacements.Count);
            for (var index = 0; index < instructions.Count; index++)
            {
                if (replacements.TryGetValue(index, out var replacement)) rewritten.AddRange(replacement);
                else rewritten.Add(instructions[index]);
            }

            rewrittenBlocks.Add(new IrBasicBlock(block.Label, rewritten));

            bool TryMatch(IrValue left, IrValue right, out IrValue? lowWord, out bool isSigned)
            {
                lowWord = null;
                isSigned = false;
                if (left.Kind != "register" || right.Kind != "register" ||
                    !bits.TryGetValue(left.RegisterName!, out var magicLow) ||
                    !biases.TryGetValue(right.RegisterName!, out isSigned))
                {
                    return false;
                }

                lowWord = magicLow;
                return true;
            }

            void Record(IrInstruction instruction)
            {
                switch (instruction)
                {
                    case IrComment:
                    case IrTracePpc:
                    case IrSetCrField:
                    case IrStore:
                        return;

                    case IrAssign assign:
                        Forget(assign.Destination);
                        if (assign.Value.Kind == "const" && assign.Value.Constant is { } constant)
                        {
                            known[assign.Destination] = unchecked((uint)constant);
                        }
                        else if (assign.Value.RegisterName is { } source && known.TryGetValue(source, out var copied))
                        {
                            known[assign.Destination] = copied;
                        }
                        return;

                    case IrBinary { Op: "fpr_from_words" } words when IsFloatRegister(words.Destination):
                        {
                            var high = words.Left.Kind == "const" && words.Left.Constant is { } h
                                ? unchecked((uint)h)
                                : words.Left.RegisterName is { } hr && known.TryGetValue(hr, out var kh) ? kh : (uint?)null;
                            Forget(words.Destination);
                            // The low word must be a value nobody rewrites later in the block: a
                            // constant, or a register that Forget drops the entry for if it changes.
                            if (high == MagicIntToDoubleHigh && words.Right.Kind is "const" or "register")
                            {
                                bits[words.Destination] = words.Right;
                            }
                            return;
                        }

                    case IrBinary binary:
                        Forget(binary.Destination);
                        return;

                    case IrLoad load:
                        Forget(load.Destination);
                        if (load.SizeBytes == 8 &&
                            IsFloatRegister(load.Destination) &&
                            GetRegisterBaseName(load.Address.Base).Equals("r2", StringComparison.OrdinalIgnoreCase) &&
                            sda2DoubleAt(load.Address.Offset) is { } value)
                        {
                            if (value == MagicSignedBias) biases[load.Destination] = true;
                            else if (value == MagicUnsignedBias) biases[load.Destination] = false;
                        }
                        return;

                    case IrCall call when call.Target.StartsWith("PPC_F", StringComparison.OrdinalIgnoreCase):
                        if (!string.IsNullOrWhiteSpace(call.Destination)) Forget(call.Destination);
                        return;

                    default:
                        known.Clear();
                        bits.Clear();
                        biases.Clear();
                        return;
                }
            }

            void Forget(string name)
            {
                known.Remove(name);
                bits.Remove(name);
                biases.Remove(name);
                List<string>? stale = null;
                foreach (var (key, lowValue) in bits)
                {
                    if (lowValue.RegisterName is { } lowName &&
                        lowName.Equals(name, StringComparison.OrdinalIgnoreCase))
                    {
                        (stale ??= new List<string>()).Add(key);
                    }
                }

                if (stale is null) return;
                foreach (var key in stale) bits.Remove(key);
            }
        }

        return changed
            ? new IrFunction(func.Name, func.EntryLabel, rewrittenBlocks)
            : func;
    }

    private static bool WritesRegister(IrFunction func, string baseRegister)
    {
        foreach (var block in func.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
                {
                    if (GetRegisterBaseName(definition).Equals(baseRegister, StringComparison.OrdinalIgnoreCase))
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    }
}
