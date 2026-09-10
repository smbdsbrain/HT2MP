# Ghidra headless post-script. Locate instruction operands that contain one of
# the requested scalar values. Useful for auditing structure-field/flag uses
# in an exact binary without mutating the analyzed project.
#
# Usage:
#   -postScript FindInstructionScalars.py <output-file> 58,100
# @category HT2MP

from ghidra.program.model.scalar import Scalar
from ghidra.program.model.address import AddressSet


args = getScriptArgs()
if len(args) < 2:
    raise ValueError("expected <output-file> <hex-scalars...>")

output_path = args[0]
scalar_text = ",".join(args[1:])
wanted = set(int(value.strip(), 16)
             for value in scalar_text.replace(" ", ",").split(",")
             if value.strip())

program = currentProgram
listing = program.getListing()
function_manager = program.getFunctionManager()

with open(output_path, "w") as output:
    for block in program.getMemory().getBlocks():
        if not block.isExecute():
            continue
        instructions = listing.getInstructions(
            AddressSet(block.getStart(), block.getEnd()), True)
        while instructions.hasNext():
            instruction = instructions.next()
            values = set()
            for operand_index in range(instruction.getNumOperands()):
                for obj in instruction.getOpObjects(operand_index):
                    if isinstance(obj, Scalar):
                        values.add(obj.getUnsignedValue())
            matched = sorted(values.intersection(wanted))
            if not matched:
                continue
            function = function_manager.getFunctionContaining(
                instruction.getAddress())
            function_name = function.getName() if function is not None else "<none>"
            raw = " ".join("%02x" % (byte_value & 0xff)
                           for byte_value in instruction.getBytes())
            output.write("%s %-22s %-24s %-8s %-36s scalars=%s\n" %
                         (instruction.getAddress(), function_name, raw,
                          instruction.getMnemonicString(),
                          instruction.toString(),
                          ",".join("0x%x" % value for value in matched)))

print("Wrote scalar-use report to %s" % output_path)
