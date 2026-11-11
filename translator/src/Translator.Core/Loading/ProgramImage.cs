using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rel;

namespace Translator.Core.Loading;

public sealed class ProgramImage
{
    public ProgramImage(
        byte[] memory,
        AddressRange usedRange,
        AddressRange dolRange,
        AddressRange relRange,
        string sha256,
        uint memoryBase = MemoryLayout.RamBase,
        AddressRange extraRange = default,
        int extraOffset = 0)
    {
        Memory = memory;
        UsedRange = usedRange;
        DolRange = dolRange;
        RelRange = relRange;
        Sha256 = sha256;
        MemoryBase = memoryBase;
        ExtraRange = extraRange;
        ExtraOffset = extraOffset;
        WindowLength = extraRange.End > extraRange.Start ? extraOffset : memory.Length;
    }

    // A second guest window stored after the main one in Memory (the linked REL region, far above
    // MEM1): guest ExtraRange.Start lives at Memory[ExtraOffset].
    public AddressRange ExtraRange { get; }
    public int ExtraOffset { get; }
    private int WindowLength { get; }

    public byte[] Memory { get; }
    public uint MemoryBase { get; }
    public uint MemoryEnd => checked(MemoryBase + (uint)WindowLength);
    public AddressRange UsedRange { get; }
    public AddressRange DolRange { get; }
    public AddressRange RelRange { get; }
    public bool HasRel => RelRange.End > RelRange.Start;
    public string Sha256 { get; }

    public bool Contains(uint address, int size = 1)
    {
        if (size < 0)
        {
            return false;
        }

        if (ExtraRange.End > ExtraRange.Start && address >= ExtraRange.Start)
        {
            return (ulong)address - ExtraRange.Start + (uint)size <= ExtraRange.Length;
        }

        if (address < MemoryBase)
        {
            return false;
        }

        var offset = (ulong)address - MemoryBase;
        return offset + (uint)size <= (ulong)WindowLength;
    }

    public int GetOffset(uint address, int size = 1)
    {
        if (!Contains(address, size))
        {
            throw new ArgumentOutOfRangeException(
                nameof(address),
                $"Guest range 0x{address:X8}+0x{size:X} is outside 0x{MemoryBase:X8}-0x{MemoryEnd:X8}.");
        }

        if (ExtraRange.End > ExtraRange.Start && address >= ExtraRange.Start)
        {
            return checked(ExtraOffset + (int)(address - ExtraRange.Start));
        }

        return checked((int)(address - MemoryBase));
    }
}
