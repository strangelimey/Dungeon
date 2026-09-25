# ============================================================================
# block_heredoc.py - PreToolUse hook on the Bash tool.
#
# Refuses any Bash command that contains a heredoc. In this environment a
# heredoc strips ONE level of backslash on its way in, even when the delimiter
# is quoted, so a patch script's "\\n" lands as a real newline and "\\x" as
# "\x" - and the script still reports "ok". Five real bugs came from it (see
# CLAUDE.md "Workflow conventions"). Knowing about the trap did not stop it,
# so this hook does.
#
# Detection is precise rather than a bare "<<" match, so C++ in a grep pattern
# ("std::cout << x") is not caught: a heredoc needs BOTH an operator naming a
# delimiter AND a later line consisting of that delimiter alone.
#
# Exit 2 blocks the call and feeds stderr back to Claude as the reason.
# ============================================================================

import json
import re
import sys

OPERATOR = re.compile(r"<<(?!<)-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1")

REASON = (
	"Blocked: this Bash command contains a heredoc. Heredocs strip one level of "
	"backslash in this environment, so escapes and Windows paths arrive mangled "
	"while the script still reports success (CLAUDE.md, Workflow conventions). "
	"Make file changes with the Edit/Write tools. If a script is genuinely needed, "
	"Write it to a .py file in the scratchpad and run it by path; for a commit "
	"message, Write it to a file and use `git commit -F <file>`."
)


def has_heredoc(command):
	lines = command.splitlines()
	for i, line in enumerate(lines):
		for m in OPERATOR.finditer(line):
			delim = m.group(2)
			for later in lines[i + 1:]:
				if later.strip() == delim:
					return True
	return False


def main():
	try:
		payload = json.loads(sys.stdin.buffer.read().decode("utf-8"))
	except Exception:
		return 0  # not ours to judge; never wedge the tool on a parse failure
	command = (payload.get("tool_input") or {}).get("command") or ""
	if has_heredoc(command):
		sys.stderr.write(REASON + "\n")
		return 2
	return 0


if __name__ == "__main__":
	sys.exit(main())
