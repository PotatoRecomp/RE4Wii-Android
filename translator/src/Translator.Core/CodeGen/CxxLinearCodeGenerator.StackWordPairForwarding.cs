using System;
using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    private readonly record struct StackWordStore(int Index, string Base, IrValue Source, bool ReadBeforeForward);

    /// <summary>
    /// The compiler's int-to-double idiom builds a double on the stack from two words and reads it
    /// straight back: <c>stw r0(0x43300000),8(r1); stw r3^0x80000000,12(r1); lfd f1,8(r1)</c>.
    /// Emitted as written, that is two byte-swapped 32-bit stores feeding one byte-swapped 64-bit
    /// load, which no ARM or x86 core forwards from the store buffer (a stall of a dozen cycles or
    /// more), plus the swaps. The load is replaced by the two stored words joined into the double's
    /// bits (<c>fpr_from_words</c>), exact because lfd is a raw 64-bit copy. The stores stay unless
    /// nothing can read the slot afterwards, the same proof the fctiw elision uses.
    ///
    /// Only stack slots addressed from the same r1 spelling are forwarded, and the window between
    /// the stores and the load may hold only loads and register arithmetic: any other store (it might
    /// alias the stack through another register), any call, any r1 update or any unmodeled
    /// instruction forgets every recorded word.
    /// </summary>
    private static IrFunction ForwardStackWordPairLoads(IrFunction func, RepresentationEnvironment types)
    {
        var stackFacts = StackAddressFacts.Build(func);
        IrCfg? cfg = null;
        var changed = false;
        var nextTemp = 0;
        var rewrittenBlocks = new List<IrBasicBlock>(func.Blocks.Count);

        foreach (var block in func.Blocks)
        {
            var instructions = block.Instructions;
            var words = new Dictionary<int, StackWordStore>();
            var storeTemps = new Dictionary<int, string>();
            var insertedBefore = new Dictionary<int, IrInstruction>();
            var replacements = new Dictionary<int, IrInstruction>();
            var removals = new HashSet<int>();

            for (var index = 0; index < instructions.Count; index++)
            {
                switch (instructions[index])
                {
                    case IrStore store when IsStackRootBase(store.Address.Base):
                        {
                            var offset = store.Address.Offset;
                            ForgetOverlapping(offset, store.SizeBytes);
                            if (store.SizeBytes == 4 &&
                                store.Source.Kind is "register" or "const" &&
                                !(store.Source.RegisterName is { } source && IsFloatRegister(source)))
                            {
                                words[offset] = new StackWordStore(index, store.Address.Base, store.Source, false);
                            }
                            break;
                        }

                    case IrLoad load when IsStackRootBase(load.Address.Base):
                        {
                            var offset = load.Address.Offset;
                            if (load.SizeBytes == 8 &&
                                IsFloatRegister(load.Destination) &&
                                words.TryGetValue(offset, out var high) &&
                                words.TryGetValue(offset + 4, out var low) &&
                                high.Base.Equals(load.Address.Base, StringComparison.Ordinal) &&
                                low.Base.Equals(load.Address.Base, StringComparison.Ordinal))
                            {
                                var highValue = CapturedWord(high);
                                var lowValue = CapturedWord(low);
                                replacements[index] = new IrBinary(load.Destination, highValue, lowValue, "fpr_from_words");
                                changed = true;

                                cfg ??= IrCfg.Build(func);
                                if (stackFacts.TryResolve(load.Address, out var slotOffset) &&
                                    CanRemoveStackSlotStore(func, cfg, block.Label, index + 1, stackFacts, slotOffset, 8))
                                {
                                    if (!high.ReadBeforeForward) removals.Add(high.Index);
                                    if (!low.ReadBeforeForward) removals.Add(low.Index);
                                }

                                // The words stay in memory (or are provably dead); later reads of the
                                // slot go through the normal load path.
                                words.Remove(offset);
                                words.Remove(offset + 4);
                                break;
                            }

                            MarkRead(offset, load.SizeBytes);
                            if (DefinesStackRoot(load.Destination)) words.Clear();
                            break;
                        }

                    case IrLoad load:
                        if (DefinesStackRoot(load.Destination)) words.Clear();
                        break;

                    case IrAssign assign:
                        if (DefinesStackRoot(assign.Destination)) words.Clear();
                        break;

                    case IrBinary binary:
                        if (DefinesStackRoot(binary.Destination)) words.Clear();
                        break;

                    case IrComment:
                    case IrTracePpc:
                    case IrSetCrField:
                        break;

                    default:
                        words.Clear();
                        break;
                }
            }

            if (replacements.Count == 0)
            {
                rewrittenBlocks.Add(block);
                continue;
            }

            var rewritten = new List<IrInstruction>(instructions.Count + insertedBefore.Count);
            for (var index = 0; index < instructions.Count; index++)
            {
                if (insertedBefore.TryGetValue(index, out var inserted))
                {
                    rewritten.Add(inserted);
                }

                if (removals.Contains(index))
                {
                    continue;
                }

                rewritten.Add(replacements.TryGetValue(index, out var replacement)
                    ? replacement
                    : instructions[index]);
            }

            rewrittenBlocks.Add(new IrBasicBlock(block.Label, rewritten));

            IrValue CapturedWord(StackWordStore word)
            {
                if (word.Source.Kind != "register")
                {
                    return word.Source;
                }

                if (!storeTemps.TryGetValue(word.Index, out var temp))
                {
                    temp = $"stackword{nextTemp++}";
                    storeTemps[word.Index] = temp;
                    insertedBefore[word.Index] = new IrAssign(temp, word.Source);
                    types.Unify(temp, ValueRepresentation.UInt32);
                }

                return IrValue.Register(temp);
            }

            void ForgetOverlapping(int offset, int size)
            {
                List<int>? stale = null;
                foreach (var (wordOffset, _) in words)
                {
                    if (RangesOverlap(offset, size, wordOffset, 4))
                    {
                        (stale ??= new List<int>()).Add(wordOffset);
                    }
                }

                if (stale is null) return;
                foreach (var wordOffset in stale) words.Remove(wordOffset);
            }

            void MarkRead(int offset, int size)
            {
                List<int>? read = null;
                foreach (var (wordOffset, word) in words)
                {
                    if (!word.ReadBeforeForward && RangesOverlap(offset, size, wordOffset, 4))
                    {
                        (read ??= new List<int>()).Add(wordOffset);
                    }
                }

                if (read is null) return;
                foreach (var wordOffset in read) words[wordOffset] = words[wordOffset] with { ReadBeforeForward = true };
            }
        }

        return changed
            ? new IrFunction(func.Name, func.EntryLabel, rewrittenBlocks)
            : func;
    }

    private static bool IsStackRootBase(string? name) =>
        !string.IsNullOrWhiteSpace(name) &&
        GetRegisterBaseName(name).Equals("r1", StringComparison.OrdinalIgnoreCase);

    private static bool DefinesStackRoot(string? destination) => IsStackRootBase(destination);
}
