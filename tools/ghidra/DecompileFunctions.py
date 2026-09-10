# Ghidra headless post-script. Export selected functions and their direct call
# graph without modifying the project.
#
# Usage:
#   -postScript DecompileFunctions.py <out-dir> 0046d710,00473470,...
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
function_manager = program.getFunctionManager()
address_space = program.getAddressFactory().getDefaultAddressSpace()
monitor = ConsoleTaskMonitor()
decompiler = DecompInterface()
decompiler.openProgram(program)

summary_path = os.path.join(out_dir, "functions.txt")
with open(summary_path, "w") as summary:
    for value in addresses:
        address = address_space.getAddress(value)
        function = function_manager.getFunctionContaining(address)
        if function is None:
            summary.write("%08x: no containing function\n" % value)
            continue

        callers = sorted(set(item.getName() for item in function.getCallingFunctions(monitor)))
        callees = sorted(set(item.getName() for item in function.getCalledFunctions(monitor)))
        entry = function.getEntryPoint()
        summary.write("%s %s size=%d\n" %
                      (entry, function.getName(), function.getBody().getNumAddresses()))
        summary.write("  callers: %s\n" % ", ".join(callers))
        summary.write("  callees: %s\n" % ", ".join(callees))

        result = decompiler.decompileFunction(function, 120, monitor)
        code = (result.getDecompiledFunction().getC()
                if result is not None and result.decompileCompleted()
                else "// decompilation failed\n")
        output_path = os.path.join(out_dir, "%s_%s.c" % (function.getName(), entry))
        with open(output_path, "w") as output:
            output.write("// callers: %s\n// callees: %s\n\n" %
                         (", ".join(callers), ", ".join(callees)))
            output.write(code)

print("Exported %d requested functions to %s" % (len(addresses), out_dir))
