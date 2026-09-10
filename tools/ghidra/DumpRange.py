# Ghidra headless Jython: dump instructions in a raw address interval.
# @category HT2MP
args = getScriptArgs()
outpath = args[0]
af = currentProgram.getAddressFactory().getDefaultAddressSpace()
listing = currentProgram.getListing()
start = af.getAddress(args[1])
end = af.getAddress(args[2])
fh = open(outpath, "w")
ins = listing.getInstructionAt(start)
if ins is None:
    ins = listing.getInstructionAfter(start)
while ins is not None and ins.getAddress().compareTo(end) < 0:
    raw = " ".join("%02x" % (b & 0xff) for b in ins.getBytes())
    fh.write("%s  %-30s  %s\n" % (ins.getAddress(), raw, ins))
    ins = ins.getNext()
fh.close()
