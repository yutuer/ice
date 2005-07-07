#!/usr/bin/env python
# **********************************************************************
#
# Copyright (c) 2003-2005 ZeroC, Inc. All rights reserved.
#
# This copy of Ice is licensed to you under the terms described in the
# ICEE_LICENSE file included in this distribution.
#
# **********************************************************************

import os, sys, shutil, re, signal, time, string

progname = os.path.basename(sys.argv[0])
infile = ""
classname = ""
lineNum = 0
errors = 0
outputFiles = [] 

def usage():
    global progname
    print >> sys.stderr, "Usage: " + progname + " [--{cpp|java} file]"

def fileError(msg):
    global progname, infile, lineNum, errors
    print >> sys.stderr, progname + ": " + infile + ':' + str(lineNum) + ": " + msg
    errors += 1

def progError(msg):
    global progname
    print >> sys.stderr, progname + ": " + msg

def removeOutputFiles():
    global outputFiles
    for entry in outputFiles:
	try:
	    if os.path.exists(entry[0]):
		os.remove(entry[0])
	except EnvironmentError, ex:
	    progError("warning: could not unlink `" + entry[0] + "': " + ex.strerror + \
	    	" -- generated file contains errors");

def handler(signum, frame):
    removeOutputFiles()
    sys.exit(128 + signum)

def openOutputFile(filename):
    global outputFiles
    try:
        outfile = file(filename, 'w')
	outputFiles.append([filename, outfile])
    except IOError, ex:
        progError("cannot open `" + filename + "' for writing: " + ex.strerror)
	removeOutputFiles()
	sys.exit(1)

def writePreamble(lang):
    global progname
    global infile
    global outputFiles
    global classname

    for entry in outputFiles:
	file = entry[1]
	file.write("// **********************************************************************\n")
	file.write("//\n")
	file.write("// Copyright (c) 2003-2005 ZeroC, Inc. All rights reserved.\n")
	file.write("//\n")
	file.write("// This copy of IceE is licensed to you under the terms described in the\n")
	file.write("// ICEE_LICENSE file included in this distribution.\n")
	file.write("//\n")
	file.write("// **********************************************************************\n")
	file.write("\n")
	file.write("// Generated by " + progname + " from file `" + infile + "', " + time.ctime() + "\n")
	file.write("\n")
	file.write("// IMPORTANT: Do not edit this file -- any edits made here will be lost!\n");
	if lang == "cpp":
	    continue
	if lang == "java":
	    file.write("\n");
	    file.write("package IceInternal;\n")
	    file.write("\n")
	    file.write("public final class " + classname + '\n');
	    file.write("{\n")
	    continue
	progError("Internal error: impossible language: `" + lang + "'")
	sys.exit(1)

    if lang == "cpp":
        header = outputFiles[1][1]
	header.write("\n");
	header.write("#ifndef ICEE_INTERNAL_" + classname + "_H\n");
	header.write("#define ICEE_INTERNAL_" + classname + "_H\n");
	header.write("\n")
	header.write("#include <IceE/Config.h>")
	header.write("\n")
	header.write("namespace IceInternal\n")
	header.write("{\n")
	header.write("\n")
	header.write("class " + classname + '\n')
	header.write("{\n")
	header.write("public:\n")
	header.write("\n")
	file = outputFiles[0][1]
	file.write("\n");
	file.write("#include <IceE/" + classname + ".h>\n")

def writePostamble(lang, labels):
    file = outputFiles[0][1]
    if lang == "cpp":
        header = outputFiles[1][1]
	header.write("\n")
	header.write("    ICEE_API static const char* const* validProps[];\n")
	header.write("};\n")
	header.write("\n")
	header.write("}\n")
	header.write("\n")
	header.write("#endif\n");
	file.write("\n");
        file.write("ICEE_API const char* const* IceInternal::" + classname + "::validProps[] =\n")
	file.write("{\n")
	for label, line in labels.iteritems():
	    file.write("    " + label + "Props,\n")
	file.write("    0\n");
	file.write("};\n")
	return
    if lang == "java":
        file.write("    public static final String[] validProps[] =\n")
	file.write("    {\n")
	for label, line in labels.iteritems():
	    file.write("        " + label + "Props,\n")
	file.write("        null\n")
	file.write("    };\n");
        file.write("}\n");
        return

def startSection(lang, label):
    if lang == "cpp":
        header = outputFiles[1][1]
	header.write("    static const char* " + label + "Props[];\n")

    file = outputFiles[0][1]
    if lang == "cpp":
	file.write("\n");
        file.write("const char* IceInternal::" + classname + "::" + label + "Props[] =\n")
        file.write("{\n");
	return
    if lang == "java":
        file.write("    public static final String " + label + "Props[] =\n");
	file.write("    {\n")
	return

def endSection(lang):
    file = outputFiles[0][1]
    if lang == "cpp":
        file.write("    0\n");
        file.write("};\n");
	return
    if lang == "java":
	file.write("        null\n");
	file.write("    };\n");
        file.write("\n")
	return

wildcard = re.compile(".*<any>.*")

def writeEntry(lang, label, entry):
    file = outputFiles[0][1]
    if lang == "cpp":
	file.write("    \"" + label + '.' + string.replace(entry, "<any>", "*") + "\",\n")
    elif lang == "java":
	pattern = string.replace(entry, ".", "\\\\.")
	pattern = string.replace(pattern, "<any>", "[^\\\\s.]+")
	file.write("        " + "\"^" + label + "\\\\." + pattern + "$\",\n")

def processFile(lang):

    #
    # Open input file.
    #
    global infile
    try:
	f = file(infile, 'r')
    except IOError, ex:
	progError("cannot open `" + infile + "': " + ex.strerror)
	sys.exit(1)

    #
    # Set up regular expressions for empty and comment lines, section headings, and entry lines.
    #
    ignore = re.compile("^\s*(?:#.*)?$") 			# Empty line or comment line
    section = re.compile("^\s*([a-zA-z_]\w*)\s*:\s*$")		# Section heading
    entry = re.compile("^\s*([^ \t\n\r\f\v#]+)(?:\s*#.*)?$")	# Any non-whitespace character sequence, except for #

    #
    # Install signal handler so we can remove the output files if we are interrupted.
    #
    signal.signal(signal.SIGINT, handler)
    signal.signal(signal.SIGHUP, handler)
    signal.signal(signal.SIGTERM, handler)

    #
    # Open output files.
    #
    global classname
    classname, ext = os.path.splitext(os.path.basename(infile))
    openOutputFile(classname + '.' + lang)
    if(lang == "cpp"):
	openOutputFile(classname + ".h")

    labels = {}		# Records the line number on which each label is defined
    atSectionStart = 0	# True for the line on which a label is defined
    seenSection = 0	# Set to true (and the remains as true) once the first label is defined
    numEntries = 0	# Number of entries within a section
    errors = 0		# Number of syntax errors in the input file

    #
    # Write preamble.
    #
    writePreamble(lang)

    #
    # Loop over lines in input file.
    #
    global lineNum
    lines = f.readlines()
    for l in lines:
	lineNum += 1

	#
	# Ignore empty lines and comments.
	#
	if ignore.match(l) != None:
	    continue

	#
	# Start of section.
	#
	labelMatch = section.match(l)
	if labelMatch != None:
	    if atSectionStart:
		fileError("section `" + label + "' must have at least one entry")
	    label = labelMatch.group(1)
	    try:
		badLine = labels[label]
		fileError("duplicate section heading: `" + label + "': previously defined on line " + badLine)
	    except KeyError:
		pass
	    if label == "validProps":
	       fileError("`validProps' is reserved and cannot be used as a section heading")
	    labels[label] = lineNum
	    if seenSection:
		endSection(lang)
	    numEntries = 0
	    startSection(lang, label)
	    seenSection = 1
	    atSectionStart = 1
	    continue

	entryMatch = entry.match(l)
	if entryMatch != None:
	    writeEntry(lang, label, entryMatch.group(1))
	    atSectionStart = 0
	    numEntries += 1
	    continue

	fileError("syntax error")

    if len(labels) == 0:
	fileError("input must define at least one section");

    #
    # End the final section.
    #
    if numEntries == 0:
	fileError("section `" + label + "' must have at least one entry")
    endSection(lang)

    #
    # End the source files.
    #
    writePostamble(lang, labels)

    global outputFiles
    for entry in outputFiles:
        entry[1].close()

    #
    # Remove the output files if anything went wrong, so we don't leave partically written files behind.
    #
    if errors != 0:
	removeOutputFiles()
	sys.exit(1)
    outputFiles = []

#
# Check arguments.
#
if len(sys.argv) != 1 and len(sys.argv) != 3:
    usage()
    sys.exit(1)

lang = ""
if len(sys.argv) == 1:
    #
    # Find where the root of the tree is.
    #
    for toplevel in [".", "..", "../..", "../../..", "../../../.."]:
	toplevel = os.path.normpath(toplevel)
	if os.path.exists(os.path.join(toplevel, "config", "makeprops.py")):
	    break
    else:
	progError("cannot find top-level directory")
	sys.exit(1)

    infile = os.path.join(toplevel, "config", "PropertyNames.def")
    lang = "all"

else:
    option = sys.argv[1]
    if option == "--cpp":
	lang = "cpp"
    elif option == "--java":
	lang = "java"
    elif option == "-h" or option == "--help" or option == "-?":
	usage()
	sys.exit(0)
    else:
	usage()
	sys.exit(1)
    infile = sys.argv[2]

if lang == "all":
    processFile("cpp")
    shutil.move("PropertyNames.cpp", os.path.join(toplevel, "src", "IceE"))
    shutil.move("PropertyNames.h", os.path.join(toplevel, "src", "IceE"))
    processFile("java")
    if os.path.exists(os.path.join(toplevel, "..", "iceje")):
        shutil.move("PropertyNames.java", os.path.join(toplevel, "..", "iceje", "src", "IceInternal"));
else:
    processFile(lang)


sys.exit(0)
