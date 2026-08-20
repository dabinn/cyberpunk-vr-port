using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace CyberpunkVRPort.AutoInstaller
{
    internal sealed class IniDocument
    {
        private readonly Dictionary<string, List<KeyValuePair<string, string>>> sections =
            new Dictionary<string, List<KeyValuePair<string, string>>>(StringComparer.OrdinalIgnoreCase);

        internal static IniDocument Load(string path)
        {
            var document = new IniDocument();
            var section = string.Empty;
            foreach (var rawLine in File.ReadAllLines(path, Encoding.UTF8))
            {
                var line = rawLine.Trim();
                if (line.Length == 0 || line.StartsWith("#") || line.StartsWith(";")) continue;
                if (line.StartsWith("[") && line.EndsWith("]") && line.Length > 2)
                {
                    section = line.Substring(1, line.Length - 2).Trim();
                    document.EnsureSection(section);
                    continue;
                }

                var equals = line.IndexOf('=');
                if (equals <= 0) continue;
                document.EnsureSection(section).Add(new KeyValuePair<string, string>(
                    line.Substring(0, equals).Trim(),
                    line.Substring(equals + 1).Trim().Replace("\\n", Environment.NewLine)));
            }
            return document;
        }

        internal IReadOnlyList<KeyValuePair<string, string>> GetSection(string name)
        {
            List<KeyValuePair<string, string>> entries;
            return sections.TryGetValue(name, out entries)
                ? entries
                : (IReadOnlyList<KeyValuePair<string, string>>)Array.Empty<KeyValuePair<string, string>>();
        }

        internal string Get(string section, string key, string fallback = "")
        {
            foreach (var entry in GetSection(section))
                if (entry.Key.Equals(key, StringComparison.OrdinalIgnoreCase)) return entry.Value;
            return fallback;
        }

        private List<KeyValuePair<string, string>> EnsureSection(string name)
        {
            List<KeyValuePair<string, string>> entries;
            if (!sections.TryGetValue(name, out entries))
            {
                entries = new List<KeyValuePair<string, string>>();
                sections[name] = entries;
            }
            return entries;
        }
    }
}
