#pragma once
namespace offcut
{
// Returns -1 when argv is not one of these commands. Explicit commands may
// contact the selected provider; tests use only an isolated keyless loopback entry.
int director_command(int argc, char** argv);
}  // namespace offcut
