# Ghidra headless Jython post-script. Disassemble and define a function at each
# given address when the project has no function there yet (for example an
# MFC message-map handler or a vtable slot the auto-analysis never followed),
# then export the same details as ExportFunctionDetails.py. Run it only with
# -readOnly: the created functions must never be committed to the curated
# project.
#
# Usage:
#   -postScript DecompileUndefined.py <out-dir> 00569650,005dd330
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
        if listing.getInstructionAt(address) is None:
            disassemble(address)
        function = createFunction(address, None)
        if function is None:
            function = function_manager.getFunctionContaining(address)
    if function is None:
        print("no function could be defined at %s" % address)
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
    print("Exported %s" % output_path)

print("Processed %d addresses into %s" % (len(addresses), out_dir))
