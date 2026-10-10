// claude-usage.exe: the CLI on its own, as a console program — so `claude-usage status` in a terminal prints in place and
// the shell waits for it. The tray executable runs the same commands; this is the one to put on PATH.
#include <windows.h>

#include <string>
#include <vector>

#include "cli.h"
#include "util.h"

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        args.push_back(util::utf8(argv[i]));
    }
    return cli::run(args);
}
