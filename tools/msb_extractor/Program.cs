using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SoulsFormats;

// Extract MSB files from ER encrypted archives
// 1. Decrypt BHD with RSA key (from Andre.Formats ArchiveKeys)
// 2. Parse as BHD5 to get file offsets + AES keys
// 3. Read + decrypt files from BDT via FileStream

// Elden Ring BHD5 RSA public keys (from Smithbox/Andre.Formats ArchiveKeys.cs)
// Two key sets: original (pre-DLC) and patched (post-DLC). Try both.
Dictionary<string, string> ErKeys1 = new()
{
    ["Data0"] = "-----BEGIN RSA PUBLIC KEY-----\nMIIBCwKCAQEA9Rju2whruXDVQZpfylVEPeNxm7XgMHcDyaaRUIpXQE0qEo+6Y36L\nP0xpFvL0H0kKxHwpuISsdgrnMHJ/yj4S61MWzhO8y4BQbw/zJehhDSRCecFJmFBz\n3I2JC5FCjoK+82xd9xM5XXdfsdBzRiSghuIHL4qk2WZ/0f/nK5VygeWXn/oLeYBL\njX1S8wSSASza64JXjt0bP/i6mpV2SLZqKRxo7x2bIQrR1yHNekSF2jBhZIgcbtMB\nxjCywn+7p954wjcfjxB5VWaZ4hGbKhi1bhYPccht4XnGhcUTWO3NmJWslwccjQ4k\nsutLq3uRjLMM0IeTkQO6Pv8/R7UNFtdCWwIERzH8IQ==\n-----END RSA PUBLIC KEY-----",
    ["Data1"] = "-----BEGIN RSA PUBLIC KEY-----\nMIIBCwKCAQEAxaBCHQJrtLJiJNdG9nq3deA9sY4YCZ4dbTOHO+v+YgWRMcE6iK6o\nZIJq+nBMUNBbGPmbRrEjkkH9M7LAypAFOPKC6wMHzqIMBsUMuYffulBuOqtEBD11\nCAwfx37rjwJ+/1tnEqtJjYkrK9yyrIN6Y+jy4ftymQtjk83+L89pvMMmkNeZaPON\n4O9q5M9PnFoKvK8eY45ZV/Jyk+Pe+xc6+e4h4cx8ML5U2kMM3VDAJush4z/05hS3\n/bC4B6K9+7dPwgqZgKx1J7DBtLdHSAgwRPpijPeOjKcAa2BDaNp9Cfon70oC+ZCB\n+HkQ7FjJcF7KaHsH5oHvuI7EZAl2XTsLEQIENa/2JQ==\n-----END RSA PUBLIC KEY-----",
    ["Data2"] = "-----BEGIN RSA PUBLIC KEY-----\nMIIBDAKCAQEA0iDVVQ230RgrkIHJNDgxE7I/2AaH6Li1Eu9mtpfrrfhfoK2e7y4O\nWU+lj7AGI4GIgkWpPw8JHaV970Cr6+sTG4Tr5eMQPxrCIH7BJAPCloypxcs2BNfT\nGXzm6veUfrGzLIDp7wy24lIA8r9ZwUvpKlN28kxBDGeCbGCkYeSVNuF+R9rN4OAM\nRYh0r1Q950xc2qSNloNsjpDoSKoYN0T7u5rnMn/4mtclnWPVRWU940zr1rymv4Jc\n3umNf6cT1XqrS1gSaK1JWZfsSeD6Dwk3uvquvfY6YlGRygIlVEMAvKrDRMHylsLt\nqqhYkZNXMdy0NXopf1rEHKy9poaHEmJldwIFAP////8=\n-----END RSA PUBLIC KEY-----",
    ["Data3"] = "-----BEGIN RSA PUBLIC KEY-----\nMIIBCwKCAQEAvRRNBnVq3WknCNHrJRelcEA2v/OzKlQkxZw1yKll0Y2Kn6G9ts94\nSfgZYbdFCnIXy5NEuyHRKrxXz5vurjhrcuoYAI2ZUhXPXZJdgHywac/i3S/IY0V/\neDbqepyJWHpP6I565ySqlol1p/BScVjbEsVyvZGtWIXLPDbx4EYFKA5B52uK6Gdz\n4qcyVFtVEhNoMvg+EoWnyLD7EUzuB2Khl46CuNictyWrLlIHgpKJr1QD8a0ld0PD\nPHDZn03q6QDvZd23UW2d9J+/HeBt52j08+qoBXPwhndZsmPMWngQDaik6FM7EVRQ\netKPi6h5uprVmMAS5wR/jQIVTMpTj/zJdwIEXszeQw==\n-----END RSA PUBLIC KEY-----",
    ["DLC"] = "-----BEGIN RSA PUBLIC KEY-----\nMIIBCwKCAQEAmYJ/5GJU4boJSvZ81BFOHYTGdBWPHnWYly3yWo01BYjGRnz8NTkz\nDHUxsbjIgtG5XqsQfZstZILQ97hgSI5AaAoCGrT8sn0PeXg2i0mKwL21gRjRUdvP\nDp1Y+7hgrGwuTkjycqqsQ/qILm4NvJHvGRd7xLOJ9rs2zwYhceRVrq9XU2AXbdY4\npdCQ3+HuoaFiJ0dW0ly5qdEXjbSv2QEYe36nWCtsd6hEY9LjbBX8D1fK3D2c6C0g\nNdHJGH2iEONUN6DMK9t0v2JBnwCOZQ7W+Gt7SpNNrkx8xKEM8gH9na10g9ne11Mi\nO1FnLm8i4zOxVdPHQBKICkKcGS1o3C2dfwIEXw/f3w==\n-----END RSA PUBLIC KEY-----",
};
Dictionary<string, string> ErKeys2 = new()
{
    ["Data0"] = "-----BEGIN RSA PUBLIC KEY-----\nMIIBDAKCAQEA7F43Ss9kroawBSUW6GBhSUo6GtxYtUV8zCPkcHhSJLGPASHhwsaX\nzMRrd+Ul9qB3oYchb4xYtdMWKFe0/ZDi9vgYXvF3rlWaZKAu1k/F6RwVAd//I3Kj\nJsYlhayskInKqB3BvB/KL2Ga8QBsZ/G9cLlUYsqIj3as9oqbfEXVmGVeuhg0I+NQ\nNL+2sThqp5eOQstfXQgqduOt0ixd/r9e5VjLhyj2z4hCEF2TVsDw9wGEBem1TkcO\nC/E8obl9fTHwEK7l2i8a4HafY7flU220r8y4UwQ+9Aq94xUYT2xdcTjdyBIaZtyS\nYmR86B680OyL9oiEonEFhh4cor/84PSmNQIFAOHX27k=\n-----END RSA PUBLIC KEY-----",
    ["Data1"] = "-----BEGIN RSA PUBLIC KEY-----\nMIIBCwKCAQEAsM5QTi6Fo1li23fkvP7jXqFlanR61VS1znElmsZH0Ez4LtuM7WUC\nQnZyi9u15T89WmIKCGpfgHZBgJEVFqW9FMhBxrA5/gXcqnGESjc+NNF71rfug/qg\nUe7B9tXlq18/bdD7qPEjYY02H5fh4Z/g0+oClSNyZR46G/MXZSw8KMV4QHikCAxJ\nN40Nxd+MpQcpc3J5SXfsXxi9gNSxHO1+KzGwRrEh1/9d7bPyd4jBuTR+SEd+ZDHR\n3jTbbRNUypB/x780KXuJnGrC8UfB6ttxfBmLs7nmhteO6R1rr5zWuHJBry7Of9t4\nJEQRDwb2VT3fpQ2oHgOc5zDYMOdObdX/tQIEZYdnGQ==\n-----END RSA PUBLIC KEY-----",
    ["Data2"] = "-----BEGIN RSA PUBLIC KEY-----\nMIIBDAKCAQEApvNV8cCaxTBtW6kB94Gd5/+NuQnVLxRo6b0QUSSXh8KGWCRNPjpq\nLEyu0kuHCTG5xfomzB5vlq9INu0odZAWZu+NWvz+YydnIQO+UPDF9J/wE92SMzBj\nn7d4uEglevPswQLiJVQThCtrl1B8dCz7vFvlSknx23jdUQ/0hfxVnLvP2GpNW/v5\niDK+J2RJFxpd8td9FpHMF+OMxT3pvQyOBleWgEcmiaA1O6AxZA3YGWaL7qnKgx4M\nPi5Ex1Cjnw66+A25kc34UvDA4pteJHC+AwTvjLN3nF2jr3l61jEcXULCWpA4rdWT\ndm77dL2KwxXDiYxNAecEFVuG/PRV7J8hHQIFAP3yuSk=\n-----END RSA PUBLIC KEY-----",
    ["Data3"] = "-----BEGIN RSA PUBLIC KEY-----\nMIIBDAKCAQEAvS4XXheufoQvvfTksJhO7JrH1ykMa2ogHtqHhAOZXorLLXvsWfVO\nQ+enFY7kBjKyVbUOlqj3M6Ho2S8QgJTO/Xz3DhD3YlHva59RnIXI2gmSyvrTB0Z/\nGopmJzVfc9o7763CMy/27tS4/dBM9qKs+csvjE6fG470Z025yECgtTtXzltg4pht\nGkjV5+tNjrFt5+wxIydNB56Xow9QCxtpZJ4TstdZvbgq1K06mpLrRTRDxpLPgdDA\n9KKwyewYliU2tl78bU5jcgL3s78KbiJ2VSrlOL2AxI6TpID+kFcDy055JsMkKR2V\nnRPreV08oQchzQ5miTezWUAk7mIcZoFHwwIFAP////8=\n-----END RSA PUBLIC KEY-----",
    ["DLC"] = "-----BEGIN RSA PUBLIC KEY-----\nMIIBCwKCAQEAmYJ/5GJU4boJSvZ81BFOHYTGdBWPHnWYly3yWo01BYjGRnz8NTkz\nDHUxsbjIgtG5XqsQfZstZILQ97hgSI5AaAoCGrT8sn0PeXg2i0mKwL21gRjRUdvP\nDp1Y+7hgrGwuTkjycqqsQ/qILm4NvJHvGRd7xLOJ9rs2zwYhceRVrq9XU2AXbdY4\npdCQ3+HuoaFiJ0dW0ly5qdEXjbSv2QEYe36nWCtsd6hEY9LjbBX8D1fK3D2c6C0g\nNdHJGH2iEONUN6DMK9t0v2JBnwCOZQ7W+Gt7SpNNrkx8xKEM8gH9na10g9ne11Mi\nO1FnLm8i4zOxVdPHQBKICkKcGS1o3C2dfwIEXw/f3w==\n-----END RSA PUBLIC KEY-----",
};

string gameDir = args.Length > 0 ? args[0] : @"G:\Steam\steamapps\common\ELDEN RING\Game";
string outDir = args.Length > 1 ? args[1] : Path.Combine(gameDir, "map", "MapStudio");

Directory.CreateDirectory(outDir);

// Copy oo2core to all possible locations Andre might search
string oo2src = Path.Combine(gameDir, "oo2core_6_win64.dll");
foreach (string dstDir in new[] {
    AppDomain.CurrentDomain.BaseDirectory,
    Environment.CurrentDirectory,
    Path.GetDirectoryName(typeof(DCX).Assembly.Location) ?? ".",
})
{
    string dst = Path.Combine(dstDir, "oo2core_6_win64.dll");
    if (File.Exists(oo2src) && !File.Exists(dst))
    {
        try { File.Copy(oo2src, dst); Console.WriteLine($"Copied oo2core to {dstDir}"); }
        catch { }
    }
}

string[] archives = { "Data0", "Data1", "Data2", "Data3", "DLC" };
int totalExtracted = 0;

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
        // Step 1: Get RSA key and decrypt BHD (try both key sets)
        MemoryStream? decryptedBhd = null;
        foreach (var keySet in new[] { ErKeys1, ErKeys2 })
        {
            if (!keySet.TryGetValue(archiveName, out string? k)) continue;
            try
            {
                decryptedBhd = SoulsFormats.Util.CryptographyUtility.DecryptRsa(bhdPath, k);
                break;
            }
            catch { }
        }
        if (decryptedBhd == null)
        {
            Console.WriteLine($"  Could not decrypt BHD");
            continue;
        }
        byte[] bhdBytes = decryptedBhd.ToArray();

        // Step 2: Parse BHD5
        BHD5 bhd5 = BHD5.Read(bhdBytes, BHD5.Game.EldenRing);
        int fileCount = bhd5.Buckets.Sum(b => b.Count);
        Console.WriteLine($"  {fileCount} entries in BHD");

        // Step 3: Read MSBs from BDT via FileStream
        int extracted = 0;
        int skipped = 0;
        int debugShown = 0;
        var magicCounts = new Dictionary<string, int>();
        var dcxInner = new Dictionary<string, int>();
        var dcxErrors = new Dictionary<string, int>();

        using var bdtStream = new FileStream(bdtPath, FileMode.Open, FileAccess.Read, FileShare.Read);

        foreach (var bucket in bhd5.Buckets)
        {
            foreach (var entry in bucket)
            {
                if (entry.FileOffset < 0 || entry.PaddedFileSize <= 0 || entry.PaddedFileSize > 100_000_000)
                    continue;

                // Read raw data from BDT
                bdtStream.Seek(entry.FileOffset, SeekOrigin.Begin);
                byte[] rawData = new byte[entry.PaddedFileSize];
                int totalRead = 0;
                while (totalRead < rawData.Length)
                {
                    int read = bdtStream.Read(rawData, totalRead, Math.Min(rawData.Length - totalRead, 8 * 1024 * 1024));
                    if (read == 0) break;
                    totalRead += read;
                }

                // Decrypt if AES-encrypted
                entry.AESKey?.Decrypt(rawData);

                // Check for DCX magic (44 43 58 00 = "DCX\0")
                if (rawData.Length < 4) continue;
                bool isDcx = rawData[0] == 0x44 && rawData[1] == 0x43 && rawData[2] == 0x58;
                // Count magic types for debug
                string m4 = System.Text.Encoding.ASCII.GetString(rawData, 0, Math.Min(4, rawData.Length)).Replace("\0", "_");
                magicCounts.TryGetValue(m4, out int mc); magicCounts[m4] = mc + 1;

                if (!isDcx) continue;

                // Decompress and check for MSB magic
                try
                {
                    byte[] decompressed = DCX.Decompress(rawData).ToArray();
                    string dm = decompressed.Length >= 4
                        ? System.Text.Encoding.ASCII.GetString(decompressed, 0, 4).Replace("\0", "_")
                        : "???";
                    dcxInner.TryGetValue(dm, out int dc); dcxInner[dm] = dc + 1;

                    if (decompressed.Length < 4 ||
                        decompressed[0] != 0x4D || decompressed[1] != 0x53 ||
                        decompressed[2] != 0x42 || decompressed[3] != 0x20)
                        continue;

                    // Parse MSBE to get map name
                    string mapName = null;
                    try
                    {
                        var msb = MSBE.Read(decompressed);
                        foreach (var model in msb.Models.MapPieces)
                        {
                            string mname = model.Name;
                            if (mname != null && mname.StartsWith("m") && mname.Contains("_"))
                            {
                                var parts = mname.Split('_');
                                if (parts.Length >= 4)
                                {
                                    mapName = $"{parts[0]}_{parts[1]}_{parts[2]}_{parts[3]}";
                                    break;
                                }
                            }
                        }
                    }
                    catch { }

                    mapName ??= $"unknown_{entry.FileNameHash:X16}";
                    string fileName = $"{mapName}.msb.dcx";
                    string destPath = Path.Combine(outDir, fileName);

                    if (!File.Exists(destPath))
                    {
                        File.WriteAllBytes(destPath, rawData);
                        extracted++;
                        if (extracted % 50 == 0)
                            Console.Write($"\r  Extracted {extracted}...");
                    }
                    else
                    {
                        skipped++;
                    }
                }
                catch (Exception dcxEx)
                {
                    dcxErrors.TryGetValue(dcxEx.GetType().Name, out int de); dcxErrors[dcxEx.GetType().Name] = de + 1;
                    if (de == 0) Console.WriteLine($"    DCX err: {dcxEx.Message[..Math.Min(dcxEx.Message.Length, 120)]}");
                }
            }
        }

        totalExtracted += extracted;
        Console.WriteLine($"\r  Extracted {extracted} MSB files ({skipped} existing)");
        Console.Write("  Outer: ");
        foreach (var kv in magicCounts.OrderByDescending(x => x.Value).Take(6))
            Console.Write($"{kv.Key}={kv.Value} ");
        Console.Write("\n  Inner: ");
        foreach (var kv in dcxInner.OrderByDescending(x => x.Value).Take(8))
            Console.Write($"{kv.Key}={kv.Value} ");
        Console.WriteLine();
    }
    catch (Exception ex)
    {
        Console.WriteLine($"  Error: {ex.Message}");
    }
}

Console.WriteLine($"\nDone: {totalExtracted} MSB files to {outDir}");
