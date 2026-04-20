// Extracts MSB files from encrypted game archives using Andre.SoulsFormats
// Build: dotnet-script extract_msb_from_archives.cs
// Or compile: csc /r:Andre.SoulsFormats.dll extract_msb_from_archives.cs

using System;
using System.IO;
using System.Linq;
using SoulsFormats;

class Program
{
    static void Main(string[] args)
    {
        string gameDir = args.Length > 0 ? args[0] : @"G:\Steam\steamapps\common\ELDEN RING\Game";
        string outDir = args.Length > 1 ? args[1] : Path.Combine(gameDir, "map", "MapStudio");

        Directory.CreateDirectory(outDir);

        string[] archives = { "Data0", "Data1", "Data2", "Data3", "DLC" };
        int totalMsb = 0;

        foreach (var archiveName in archives)
        {
            string bhdPath = Path.Combine(gameDir, archiveName + ".bhd");
            string bdtPath = Path.Combine(gameDir, archiveName + ".bdt");

            if (!File.Exists(bhdPath) || !File.Exists(bdtPath))
            {
                Console.WriteLine($"Skipping {archiveName} (not found)");
                continue;
            }

            Console.WriteLine($"Reading {archiveName}...");
            try
            {
                var bxf = BXF4.Read(bhdPath, bdtPath);
                int count = 0;
                foreach (var file in bxf.Files)
                {
                    string name = file.Name ?? "";
                    if (name.Contains("MapStudio") && name.EndsWith(".msb.dcx"))
                    {
                        string fileName = Path.GetFileName(name);
                        string destPath = Path.Combine(outDir, fileName);

                        if (!File.Exists(destPath))
                        {
                            File.WriteAllBytes(destPath, file.Bytes.ToArray());
                            count++;
                        }
                    }
                }
                totalMsb += count;
                Console.WriteLine($"  Extracted {count} new MSB files");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"  Error: {ex.Message}");
            }
        }

        Console.WriteLine($"\nTotal: {totalMsb} MSB files extracted to {outDir}");
    }
}
