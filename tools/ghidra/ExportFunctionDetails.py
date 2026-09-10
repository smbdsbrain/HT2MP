# Ghidra headless post-script. Export a decompilation plus an instruction-level
# listing for selected functions. The script is deliberately read-only and is
# intended for ABI/call-site validation where the decompiler's inferred
# arguments are not sufficient.
#
# Usage:
#   -postScript ExportFunctionDetails.py <out-dir> 0046d710,00473470,...
# @category HT2MP

import os

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


args = getScriptArgs()
if len(args) < 2:
    raise ValueError("expected <out-dir> <addresses...>")

out_dir = args[0]
address_text = ",".join(args[1:])
addresses = [int(value.strip(), 16)
             for value in address_text.replace(" ", ",").split(",")
             if value.strip()]
if not os.path.isdir(out_dir):
    os.makedirs(out_dir)

program = currentProgram
listing = program.getListing()
function_manager = program.getFunctionManager()
address_space = program.getAddressFactory().getDefaultAddressSpace()
monitor = ConsoleTaskMonitor()
decompiler = DecompInterface()
decompiler.openProgram(program)

for value in addresses:
    address = address_space.getAddress(value)
    function = function_manager.getFunctionContaining(address)
    if function is None:
        continue

    entry = function.getEntryPoint()
    output_path = os.path.join(out_dir, "%s_%s.details.txt" %
                               (function.getName(), entry))
    with open(output_path, "w") as output:
        output.write("entry: %s\n" % entry)
        output.write("name: %s\n" % function.getName())
        output.write("signature: %s\n" % function.getSignature())
        output.write("calling-convention: %s\n" %
                     function.getCallingConventionName())
        output.write("stack-purge: %s\n" % function.getStackPurgeSize())
        output.write("size: %d\n\n" % function.getBody().getNumAddresses())

        output.write("[instructions]\n")
        instructions = listing.getInstructions(function.getBody(), True)
        while instructions.hasNext():
            instruction = instructions.next()
            operands = []
            for index in range(instruction.getNumOperands()):
                operands.append(instruction.getDefaultOperandRepresentation(index))
            raw = " ".join("%02x" % (byte_value & 0xff)
                           for byte_value in instruction.getBytes())
            output.write("%s  %-24s  %-8s %s\n" %
                         (instruction.getAddress(), raw,
                          instruction.getMnemonicString(),
                          ", ".join(operands)))

        output.write("\n[decompilation]\n")
        result = decompiler.decompileFunction(function, 120, monitor)
        code = (result.getDecompiledFunction().getC()
                if result is not None and result.decompileCompleted()
                else "// decompilation failed\n")
        output.write(code)

print("Exported %d detailed functions to %s" % (len(addresses), out_dir))
