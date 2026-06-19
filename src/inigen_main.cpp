// mfg_inigen - emits a default MapForGoblins.ini from the in-code schema.
// Build-time tool so the shipped ini is generated, never hand-maintained.
//
//   mfg_inigen <out.ini> [--vanilla]
//
// --vanilla omits ERR-only sections/entries.

#include "goblin_config_schema.hpp"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    std::string out_path;
    bool vanilla = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--vanilla")
            vanilla = true;
        else if (a == "--err")
            vanilla = false;
        else if (!a.empty() && a[0] != '-')
            out_path = a;
    }

    if (out_path.empty())
    {
        std::cerr << "usage: mfg_inigen <out.ini> [--vanilla]\n";
        return 2;
    }

    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        std::cerr << "mfg_inigen: cannot write " << out_path << "\n";
        return 1;
    }

    goblin::emit_ini(f, /*include_err_only=*/!vanilla);
    std::cout << "mfg_inigen: wrote " << out_path << (vanilla ? " (vanilla)\n" : " (err)\n");
    return 0;
}
