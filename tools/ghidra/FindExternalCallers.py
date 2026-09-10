# Ghidra headless post-script. Report code references and containing functions
# for imported/external functions whose names match requested fragments.
#
# Usage: -postScript FindExternalCallers.py <out-file> GetFocus PeekMessage
# @category HT2MP

import os

args = getScriptArgs()
if len(args) < 2:
    raise ValueError("expected <out-file> <external-name-fragments...>")

out_path = args[0]
needles = [value.lower() for value in args[1:]]
parent = os.path.dirname(out_path)
if parent and not os.path.isdir(parent):
    os.makedirs(parent)

program = currentProgram
functions = program.getFunctionManager()
references = program.getReferenceManager()

matches = []
iterator = functions.getExternalFunctions()
while iterator.hasNext():
    external = iterator.next()
    name = external.getName()
    if not any(needle in name.lower() for needle in needles):
        continue
    refs = []
    ref_iterator = references.getReferencesTo(external.getEntryPoint())
    while ref_iterator.hasNext():
        reference = ref_iterator.next()
        source = reference.getFromAddress()
        owner = functions.getFunctionContaining(source)
        refs.append((source, owner.getEntryPoint() if owner else None,
                     owner.getName() if owner else "<no function>"))
    matches.append((name, external.getEntryPoint(), refs))

with open(out_path, "w") as output:
    for name, address, refs in sorted(matches):
        output.write("%s %s\n" % (address, name))
        if not refs:
            output.write("  <no references>\n")
        for source, entry, owner_name in refs:
            output.write("  %s -> %s %s\n" % (source, entry, owner_name))

print("Found %d matching externals; wrote %s" % (len(matches), out_path))
