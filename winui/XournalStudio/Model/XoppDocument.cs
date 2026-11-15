using System.IO.Compression;
using System.Text;
using System.Xml;
using System.Xml.Linq;

namespace XournalStudio.Model;

/// <summary>
/// A Xournal++ document (.xopp: gzip-compressed XML). The XML tree is kept as loaded so that
/// everything this app does not understand (text, images, layers, attributes) is written back
/// unchanged; only strokes are taken over by the ink model of each page.
/// </summary>
public sealed class XoppDocument
{
    public const double A4Width = 595.27559;
    public const double A4Height = 841.88976;

    private readonly XDocument xml;

    public string? FilePath { get; set; }

    /// <summary>Resolved path of the PDF used as page background, if any.</summary>
    public string? PdfPath { get; private set; }

    public List<XoppPage> Pages { get; } = new();

    // The PDF reference as found in the file (may be relative); written back unchanged.
    private (string? Domain, string? Filename) loadedPdfReference;

    private XElement Root => xml.Root!;

    private XoppDocument(XDocument xml)
    {
        this.xml = xml;
    }

    public static XoppDocument CreateNew()
    {
        var doc = new XoppDocument(NewXml());
        doc.Pages.Add(XoppPage.CreateGraph(A4Width, A4Height));
        return doc;
    }

    /// <summary>A document with one page per PDF page, the PDF as background.</summary>
    public static XoppDocument FromPdf(string pdfPath, IReadOnlyList<(double Width, double Height)> pageSizes)
    {
        var doc = new XoppDocument(NewXml()) { PdfPath = pdfPath };
        for (int i = 0; i < pageSizes.Count; i++)
        {
            doc.Pages.Add(XoppPage.CreatePdf(pageSizes[i].Width, pageSizes[i].Height, i));
        }
        return doc;
    }

    public static XoppDocument Load(string path)
    {
        XDocument xml;
        using (var file = File.OpenRead(path))
        {
            Stream input = file;
            int b1 = file.ReadByte(), b2 = file.ReadByte();
            file.Position = 0;
            if (b1 == 0x1f && b2 == 0x8b)
            {
                input = new GZipStream(file, CompressionMode.Decompress);
            }
            xml = XDocument.Load(input, LoadOptions.PreserveWhitespace);
        }

        if (xml.Root?.Name.LocalName != "xournal")
        {
            throw new InvalidDataException("Not a Xournal++ document.");
        }

        var doc = new XoppDocument(xml) { FilePath = path };
        foreach (var pageElement in xml.Root.Elements("page").ToList())
        {
            pageElement.Remove();
            doc.Pages.Add(XoppPage.FromXml(pageElement));
        }

        var firstPdf = doc.Pages.Select(p => p.Background).FirstOrDefault(b => (string?)b.Attribute("type") == "pdf");
        if (firstPdf != null)
        {
            doc.loadedPdfReference = ((string?)firstPdf.Attribute("domain"), (string?)firstPdf.Attribute("filename"));
            doc.PdfPath = ResolvePdfPath(path, doc.loadedPdfReference.Domain, doc.loadedPdfReference.Filename);
        }
        return doc;
    }

    /// <summary>Writes "&lt;path&gt;~" first and replaces the file only once it is complete.</summary>
    public void Save(string path)
    {
        Root.SetAttributeValue("creator", "Xournal Studio WinUI");
        if (Root.Attribute("fileversion") == null)
        {
            Root.SetAttributeValue("fileversion", "4");
        }

        var output = new XDocument(new XDeclaration("1.0", null, "no"), new XElement(Root));
        bool firstPdf = true;
        foreach (var page in Pages)
        {
            var element = new XElement(page.ToXml());
            var background = element.Element("background");
            if (background != null && (string?)background.Attribute("type") == "pdf")
            {
                // Only the first PDF background names the file; Xournal reads the attributes in this order.
                var pageNo = (string?)background.Attribute("pageno");
                background.RemoveAttributes();
                background.SetAttributeValue("type", "pdf");
                if (firstPdf && loadedPdfReference.Filename != null)
                {
                    background.SetAttributeValue("domain", loadedPdfReference.Domain ?? "absolute");
                    background.SetAttributeValue("filename", loadedPdfReference.Filename);
                }
                else if (firstPdf && PdfPath != null)
                {
                    background.SetAttributeValue("domain", "absolute");
                    background.SetAttributeValue("filename", PdfPath);
                }
                background.SetAttributeValue("pageno", pageNo);
                firstPdf = false;
            }
            output.Root!.Add(element);
        }

        string temp = path + "~";
        using (var file = File.Create(temp))
        using (var gzip = new GZipStream(file, CompressionLevel.Fastest))
        using (var writer = XmlWriter.Create(gzip, new XmlWriterSettings
        {
            Encoding = new UTF8Encoding(false),
            Indent = true,
            IndentChars = "",
        }))
        {
            output.Save(writer);
        }
        File.Move(temp, path, overwrite: true);
        FilePath = path;
    }

    private static XDocument NewXml() =>
        new(new XElement("xournal",
            new XAttribute("creator", "Xournal Studio WinUI"),
            new XAttribute("fileversion", "4"),
            new XElement("title", "Xournal++ document - see https://xournalpp.github.io/")));

    private static string? ResolvePdfPath(string xoppPath, string? domain, string? filename)
    {
        if (string.IsNullOrEmpty(filename))
        {
            return null;
        }

        string dir = Path.GetDirectoryName(Path.GetFullPath(xoppPath))!;
        var candidates = new List<string>();
        if (domain == "attach")
        {
            candidates.Add(Path.ChangeExtension(xoppPath, null) + ".xopp." + filename);
            candidates.Add(xoppPath + "." + filename);
        }
        else
        {
            candidates.Add(Path.IsPathRooted(filename) ? filename : Path.Combine(dir, filename));
            // Moved documents: Xournal also looks for the PDF next to the .xopp.
            candidates.Add(Path.Combine(dir, Path.GetFileName(filename.Replace('/', '\\'))));
        }
        return candidates.FirstOrDefault(File.Exists) ?? candidates[0];
    }
}
