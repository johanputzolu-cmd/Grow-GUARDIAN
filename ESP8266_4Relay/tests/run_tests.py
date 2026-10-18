"""Compile and execute the actual scheduler and settings functions without hardware."""

import argparse
import os
import pathlib
import shutil
import subprocess
import tempfile


def between(source, start, end):
    return source[source.index(start):source.index(end)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=shutil.which("g++"))
    args = parser.parse_args()
    if not args.compiler:
        parser.error("g++ not found; specify --compiler with a C++ compiler path")
    os.environ["PATH"] = str(pathlib.Path(args.compiler).resolve().parent) + os.pathsep + os.environ["PATH"]
    sketch = pathlib.Path(__file__).resolve().parents[1]
    source = (sketch / "ESP8266_4Relay.ino").read_text(encoding="utf-8")
    extracted = (
        between(source, "const uint32_t SETTINGS_MAGIC", "ESP8266WebServer server")
        + "\nRelaySettings settings = {};\nbool settingsSaved = true;\n"
        + between(source, "uint32_t temporaryEnd[4]", "const char* const TASK_NAMES")
        + between(source, "bool saveSettings()", "bool parseUnsigned(")
        + between(source, "bool scheduledProgramState(", "const char* modeName(")
        + between(source, "bool parseConditionNumber(", "void handleConditionControl(")
    )
    with tempfile.TemporaryDirectory(prefix="grow-scheduler-tests-") as directory:
        build = pathlib.Path(directory)
        (build / "RelayUnderTest.h").write_text(extracted, encoding="utf-8")
        executable = build / "scheduler-tests.exe"
        subprocess.run(
            [args.compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
             "-I", str(sketch), "-I", str(build),
             str(sketch / "tests" / "scheduler_test.cpp"), "-o", str(executable)],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
        condition_executable = build / "condition-tests.exe"
        subprocess.run(
            [args.compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
             "-I", str(sketch), str(sketch / "tests" / "condition_test.cpp"),
             "-o", str(condition_executable)],
            check=True,
        )
        subprocess.run([str(condition_executable)], check=True)


if __name__ == "__main__":
    main()
