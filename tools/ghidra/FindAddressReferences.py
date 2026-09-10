# Ghidra headless post-script. Report references to exact virtual addresses and
# the containing source functions without modifying the program or project.
#
# Usage:
#   -postScript FindAddressReferences.py <out-file> 0068c078 0068c27c
# @category HT2MP

import os


args = getScriptArgs()
if len(args) < 2:
    raise ValueError("expected <out-file> <addresses...>")

out_path = args[0]
address_text = ",".join(args[1:])
values = [int(value.strip(), 16)
          for value in address_text.replace(" ", ",").split(",")
          if value.strip()]
parent = os.path.dirname(out_path)
if parent and not os.path.isdir(parent):
    os.makedirs(parent)

program = currentProgram
space = program.getAddressFactory().getDefaultAddressSpace()
reference_manager = program.getReferenceManager()
function_manager = program.getFunctionManager()

with open(out_path, "w") as output:
    for value in values:
        target = space.getAddress(value)
        output.write("%s\n" % target)
        iterator = reference_manager.getReferencesTo(target)
        count = 0
        while iterator.hasNext():
            reference = iterator.next()
            source = reference.getFromAddress()
            function = function_manager.getFunctionContaining(source)
            output.write("  %s %-20s %s\n" %
                         (source,
                          function.getName() if function else "<no function>",
                          reference.getReferenceType()))
            count += 1
        if count == 0:
            output.write("  <no references>\n")

print("Wrote exact-address references to %s" % out_path)
