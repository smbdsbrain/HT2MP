# Ghidra headless postScript (Jython 2.7).
# Usage: -postScript DecompileAddresses.py <outdir> <hex-address>...
# @category HT2MP

import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
if len(args) < 2:
    raise RuntimeError("expected output directory and at least one address")

outdir = args[0]
if not os.path.isdir(outdir):
    os.makedirs(outdir)

program = currentProgram
functions = program.getFunctionManager()
space = program.getAddressFactory().getDefaultAddressSpace()
monitor = ConsoleTaskMonitor()
decompiler = DecompInterface()
decompiler.openProgram(program)

selected = {}
for value in args[1:]:
    address = space.getAddress(int(value, 16))
    function = functions.getFunctionContaining(address)
    if function is None:
        function = functions.getFunctionAt(address)
    if function is not None:
        selected[function.getEntryPoint().getOffset()] = function

summary = open(os.path.join(outdir, "_selected_callgraph.txt"), "w")
for entry in sorted(selected.keys()):
    function = selected[entry]
    callers = sorted(set(item.getName() for item in
                         function.getCallingFunctions(monitor)))
    callees = sorted(set(item.getName() for item in
                         function.getCalledFunctions(monitor)))
    summary.write("%s %-24s size=%-6d callers=[%s] callees=[%s]\n" % (
        function.getEntryPoint(), function.getName(),
        function.getBody().getNumAddresses(), ",".join(callers),
        ",".join(callees)))
    result = decompiler.decompileFunction(function, 120, monitor)
    text = (result.getDecompiledFunction().getC()
            if result is not None and result.decompileCompleted()
            else "// decompile failed\n")
    output = open(os.path.join(
        outdir, "fn_%s_%s.c" % (function.getName(),
                                 function.getEntryPoint())), "w")
    output.write("// callers=[%s]\n// callees=[%s]\n%s" % (
        ",".join(callers), ",".join(callees), text))
    output.close()
summary.close()
print("decompiled %d selected functions to %s" %
      (len(selected), outdir))
