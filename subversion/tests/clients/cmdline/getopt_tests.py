#!/usr/bin/env python
#
#  getopt_tests.py:  testing the svn command line processing
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2002 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

# General modules
import string, sys, re, os.path

# Our testing module
import svntest


######################################################################
# Tests
#
#   Each test must return 0 on success or non-zero on failure.

#----------------------------------------------------------------------

# This directory contains all the expected output from svn.
getopt_output_dir = os.path.join(os.path.dirname(sys.argv[0]),
                                 'getopt_tests_data')

# Naming convention for golden files: take the svn command line as a
# single string and apply the following sed transformations:
#   echo svn option1 option2 ... | sed -e 's/ /_/g' -e 's/_--/--/g'
# Then append either _stdout or _stderr for the file descriptor to
# compare against.

def load_expected_output(basename):
  "load the expected standard output and standard error"

  stdout_filename = os.path.join(getopt_output_dir, basename + '_stdout')
  stderr_filename = os.path.join(getopt_output_dir, basename + '_stderr')

  exp_stdout = open(stdout_filename, 'r').readlines()
  exp_stderr = open(stderr_filename, 'r').readlines()

  return exp_stdout, exp_stderr

# This is a list of lines to delete.
del_lines_res = [ re.compile(r'\s+compiled\s+'),
                  re.compile(r"- handles '(https|file)' schema"),
                ]

# This is a list of lines to search and replace text on.
rep_lines_res = [ (re.compile(r'version \d+\.\d+\.\d+ '), 'version X.Y.Z '),
                ]

def process_lines(lines):
  "delete lines that should not be compared and search and replace the rest"
  output = [ ]
  for line in lines:
    # Skip these lines from the output list.
    delete_line = 0
    for delete_re in del_lines_res:
      if delete_re.search(line, 1):
        delete_line = 1
        break
    if delete_line:
      continue

    # Search and replace text on the rest.
    for replace_re, replace_str in rep_lines_res:
      line = replace_re.sub(replace_str, line)

    output.append(line)

  return output

def run_one_test(sbox, basename, *varargs):
  "run svn with args and compare against the specified output files"

  if sbox.build():
    return 1

  exp_stdout, exp_stderr = load_expected_output(basename)

  actual_stdout, actual_stderr = apply(svntest.main.run_svn, (1,) + varargs)

  # Delete and perform search and replaces on the lines from the
  # actual and expected output that may differ between build
  # environments.
  exp_stdout    = process_lines(exp_stdout)
  exp_stderr    = process_lines(exp_stderr)
  actual_stdout = process_lines(actual_stdout)
  actual_stderr = process_lines(actual_stderr)

  if exp_stdout != actual_stdout:
    print "Standard output does not match."
    print "Expected standard output:\n", exp_stdout, "\n"
    print "Actual standard output:\n", actual_stdout, "\n"
    return 1

  if exp_stderr != actual_stderr:
    print "Standard error does not match."
    print "Expected standard output:\n", exp_stderr, "\n"
    print "Actual standard output:\n", actual_stderr, "\n"
    return 1

  return 0

def getopt_no_args(sbox):
  "run svn with no arguments"

  return run_one_test(sbox, 'svn')

def getopt__version(sbox):
  "run svn --version"

  return run_one_test(sbox, 'svn--version', '--version')

def getopt__help(sbox):
  "run svn --help"

  return run_one_test(sbox, 'svn--help', '--help')

def getopt_help(sbox):
  "run svn help"

  return run_one_test(sbox, 'svn_help', 'help')

def getopt_help__version(sbox):
  "run svn help --version"

  return run_one_test(sbox, 'svn_help--version', 'help', '--version')

def getopt_help_log_switch(sbox):
  "run svn help log switch"

  return run_one_test(sbox, 'svn_help_log_switch', 'help', 'log', 'switch')

def getopt_help_bogus_cmd(sbox):
  "run svn help bogus-cmd"

  return run_one_test(sbox, 'svn_help_bogus-cmd', 'help', 'bogus-cmd')

########################################################################
# Run the tests


# list all tests here, starting with None:
test_list = [ None,
              getopt_no_args,
              getopt__version,
              getopt__help,
              getopt_help,
              getopt_help__version,
              getopt_help_bogus_cmd,
              getopt_help_log_switch
            ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.
# local variables:
# eval: (load-file "../../../../tools/dev/svn-dev.el")
# end:
