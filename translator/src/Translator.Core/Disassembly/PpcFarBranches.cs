namespace Translator.Core.Disassembly;

/// <summary>
/// Relative b/bl instructions whose real target lies beyond the 26-bit displacement, keyed by branch
/// address. RELs linked at fixed homes in MEM2 call the DOL this way (tools/re4-rels.py writes them as
/// "far" lines of the REL layout); the displacement in the word itself is meaningless for these.
/// Set once at startup, before any decoding.
/// </summary>
public static class PpcFarBranches
{
    private static IReadOnlyDictionary<uint, uint> _targets = new Dictionary<uint, uint>();

    public static void Set(IReadOnlyDictionary<uint, uint> targets) => _targets = targets;

    public static bool TryGet(uint address, out uint target) => _targets.TryGetValue(address, out target);
}
