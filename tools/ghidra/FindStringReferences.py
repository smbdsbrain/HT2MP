# Ghidra headless post-script. Find defined strings containing one of the
# requested fragments and report every code reference plus its containing
# function. The script never changes the program or project.
#
# Usage:
#   -postScript FindStringReferences.py <out-file> processMove setPosition
# @category HT2MP

import os


args = getScriptArgs()
if len(args) < 2:
    raise ValueError("expected <out-file> <string-fragments...>")

out_path = args[0]
needles = [value.lower() for value in args[1:]]
parent = os.path.dirname(out_path)
if parent and not os.path.isdir(parent):
    os.makedirs(parent)

program = currentProgram
listing = program.getListing()
references = program.getReferenceManager()
functions = program.getFunctionManager()

matches = []
items = listing.getDefinedData(True)
while items.hasNext():
    item = items.next()
    value = item.getValue()
    if value is None:
        continue
    text = str(value)
    lowered = text.lower()
    if not any(needle in lowered for needle in needles):
        continue

    refs = []
    iterator = references.getReferencesTo(item.getMinAddress())
    while iterator.hasNext():
        reference = iterator.next()
        source = reference.getFromAddress()
        function = functions.getFunctionContaining(source)
        refs.append((source, function.getEntryPoint() if function else None,
                     function.getName() if function else "<no function>"))
    matches.append((item.getMinAddress(), text, refs))

with open(out_path, "w") as output:
    for address, value, refs in matches:
        output.write("%s %r\n" % (address, value))
        if not refs:
            output.write("  <no references>\n")
        for source, entry, name in refs:
            output.write("  %s -> %s %s\n" % (source, entry, name))

print("Found %d matching strings; wrote %s" % (len(matches), out_path))
