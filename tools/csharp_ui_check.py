#!/usr/bin/env python3
"""Check the C# language UI mode's console (src/language_ui_csharp.h) without a window.

The console program the mode builds (kReplProgram, kReplProject) is built with
the real .NET SDK and driven over stdin the way the editor drives it:
  - `:run` of a .NET file-based app whose `#:project` directive references a
    library project: the console builds the project, references its assembly,
    and the file's top-level statements use it;
  - `:evalat` of one statement of that file (the directive is honoured there too);
  - `:help` of a member of the referenced library: the Help tab shows its XML
    documentation;
  - a `#:project` naming no project is reported, not fatal.
Needs dotnet (a .NET 10 SDK) on PATH and no network. GUI coverage is manual.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / 'src/language_ui_csharp.h').read_text()
PROGRAM = HEADER.split('kReplProgram = R"MEPCS(', 1)[1].split(')MEPCS"', 1)[0]
PROJECT = HEADER.split('kReplProject = R"MEPCS(', 1)[1].split(')MEPCS"', 1)[0]

LIBRARY_PROJECT = """<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>enable</Nullable>
    <GenerateDocumentationFile>true</GenerateDocumentationFile>
  </PropertyGroup>
</Project>
"""
LIBRARY = """namespace Shapes;

/// <summary>Squares.</summary>
public static class Square
{
    /// <summary>The area of a square.</summary>
    /// <param name="side">Its side.</param>
    public static double Area(double side) => side * side;
}
"""
APP = """// CMD: dotnet run app.cs
#:project lib/Shapes.csproj
using Shapes;

var area = Square.Area(6.5);
Console.WriteLine($"area={area}");
"""
NO_SUCH = """#:project nowhere/Missing.csproj
Console.WriteLine("still runs");
"""
NUGET_CONFIG = """<?xml version="1.0" encoding="utf-8"?>
<configuration><packageSources><clear /></packageSources></configuration>
"""

ENV = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1')


class Console(unittest.TestCase):
    tmp: Path
    dll: Path

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='mep-csharp-ui-'))
        (cls.tmp / 'nuget.config').write_text(NUGET_CONFIG)
        build = cls.tmp / 'console'
        build.mkdir()
        (build / 'Program.cs').write_text(PROGRAM)
        (build / 'mep-csharp-repl.csproj').write_text(PROJECT)
        out = subprocess.run(['dotnet', 'build', str(build / 'mep-csharp-repl.csproj'), '-c', 'Release', '-o', str(cls.tmp / 'bin'),
                              '-nologo', '-v', 'q', '-nodeReuse:false', '-p:UseSharedCompilation=false'],
                             capture_output=True, text=True, env=ENV, cwd=cls.tmp)
        if out.returncode != 0:
            raise RuntimeError('building the console failed:\n' + out.stdout + out.stderr)
        cls.dll = cls.tmp / 'bin/mep-csharp-repl.dll'
        (cls.tmp / 'lib').mkdir()
        (cls.tmp / 'lib/Shapes.csproj').write_text(LIBRARY_PROJECT)
        (cls.tmp / 'lib/Square.cs').write_text(LIBRARY)
        (cls.tmp / 'app.cs').write_text(APP)
        (cls.tmp / 'nosuch.cs').write_text(NO_SUCH)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def console(self, *commands):
        session = Path(tempfile.mkdtemp(dir=self.tmp, prefix='session-'))
        out = subprocess.run(['dotnet', str(self.dll), str(session)], input='\n'.join(commands) + '\n:quit\n',
                             capture_output=True, text=True, env=ENV, cwd=self.tmp, timeout=600)
        return out.stdout + out.stderr, session

    def test_run_references_the_project(self):
        out, session = self.console(f':run {self.tmp / "app.cs"}', ':help Shapes.Square.Area')
        self.assertIn('area=42.25', out)
        self.assertIn('referenced', out)
        self.assertNotIn('not supported', out)
        help_text = (session / 'help.txt').read_text()
        self.assertIn('The area of a square.', help_text)

    def test_evalat_references_the_project(self):
        out, _ = self.console(f':evalat {self.tmp / "app.cs"} 5', 'Console.WriteLine(area + 1);')
        self.assertIn('43.25', out)

    def test_missing_project_is_reported(self):
        out, _ = self.console(f':run {self.tmp / "nosuch.cs"}')
        self.assertIn('no such project', out)
        self.assertIn('still runs', out)


if __name__ == '__main__':
    if shutil.which('dotnet') is None:
        sys.exit('csharp_ui_check: needs dotnet on PATH')
    unittest.main(verbosity=2)
