using System.IO.Compression;
using System.Xml.Linq;
using XournalStudio.Model;
using Xunit;

namespace XournalStudio.Tests;

public class FileFormatTests : IDisposable
{
    private readonly string dir = Path.Combine(Path.GetTempPath(), "xs-tests-" + Guid.NewGuid().ToString("N"));

    public FileFormatTests() => Directory.CreateDirectory(dir);

    public void Dispose() => Directory.Delete(dir, true);

    private string Write(string name, string xml, bool gzip = true)
    {
        string path = Path.Combine(dir, name);
        using var file = File.Create(path);
        using Stream output = gzip ? new GZipStream(file, CompressionMode.Compress) : file;
        using var writer = new StreamWriter(output);
        writer.Write(xml);
        return path;
    }

    private static XDocument ReadXml(string path)
    {
        using var file = File.OpenRead(path);
        using var gzip = new GZipStream(file, CompressionMode.Decompress);
        return XDocument.Load(gzip);
    }

    private const string Sample = """
        <?xml version="1.0" standalone="no"?>
        <xournal creator="xournalpp 1.3.8+dev" fileversion="5" fork-export="x_kommentiert.pdf">
        <title>Xournal++ document - see https://xournalpp.github.io/</title>
        <page width="595.27559" height="841.88976">
        <background type="pdf" domain="absolute" filename="lecture.pdf" pageno="1"/>
        <layer>
        <stroke tool="pen" ts="0" fn="" color="#3333ccff" width="1.41 1.1 1.2" capStyle="round">10 20 30 40 50 60</stroke>
        <text font="Sans" size="12" x="10" y="10" color="#000000ff">Hallo</text>
        </layer>
        </page>
        <page width="595.27559" height="841.88976">
        <background type="pdf" pageno="2"/>
        <layer name="Ebene 2"><stroke tool="highlighter" color="#ffff007f" width="8.5">1 2 3 4</stroke></layer>
        </page>
        <page width="595.27559" height="841.88976">
        <background type="solid" color="#ffffffff" style="graph"/>
        <layer/>
        </page>
        </xournal>
        """;

    [Fact]
    public void LoadsPagesBackgroundsAndPdfReference()
    {
        File.WriteAllText(Path.Combine(dir, "lecture.pdf"), "");
        var doc = XoppDocument.Load(Write("a.xopp", Sample));

        Assert.Equal(3, doc.Pages.Count);
        Assert.True(doc.Pages[0].IsPdf);
        Assert.Equal(1, doc.Pages[1].PdfPageIndex);
        Assert.Equal(BackgroundStyle.Graph, doc.Pages[2].Style);
        Assert.Equal(Path.Combine(dir, "lecture.pdf"), doc.PdfPath);
        Assert.Single(doc.Pages[0].Strokes);
        Assert.True(doc.Pages[1].Strokes[0].DrawingAttributes.DrawAsHighlighter);
    }

    [Fact]
    public void UnchangedDocumentIsWrittenBackWithEverythingItContains()
    {
        string path = Write("b.xopp", Sample);
        var before = ReadXml(path);
        XoppDocument.Load(path).Save(path);
        var after = ReadXml(path);

        Assert.Equal("5", (string?)after.Root!.Attribute("fileversion"));
        Assert.Equal("x_kommentiert.pdf", (string?)after.Root.Attribute("fork-export"));
        var pagesBefore = before.Root!.Elements("page").ToList();
        var pagesAfter = after.Root.Elements("page").ToList();
        Assert.Equal(pagesBefore.Count, pagesAfter.Count);
        for (int i = 0; i < pagesBefore.Count; i++)
        {
            Assert.True(XNode.DeepEquals(Normalize(pagesBefore[i]), Normalize(pagesAfter[i])), $"page {i + 1}");
        }
        Assert.False(File.Exists(path + "~"));
    }

    [Fact]
    public void ReadsPlainXmlToo()
    {
        var doc = XoppDocument.Load(Write("c.xopp", Sample, gzip: false));
        Assert.Equal(3, doc.Pages.Count);
    }

    [Fact]
    public void StrokeRoundTripKeepsPointsColorAndWidths()
    {
        var e = XElement.Parse("""<stroke tool="pen" color="#ff0000ff" width="2 1.5 2.5 3">1.5 2.5 10 20 30 40 50 60</stroke>""");
        var stroke = StrokeConverter.FromXml(e)!;
        var back = StrokeConverter.ToXml(stroke);

        Assert.Equal("pen", (string?)back.Attribute("tool"));
        Assert.Equal("#ff0000ff", (string?)back.Attribute("color"));
        Assert.Equal("1.5 2.5 10 20 30 40 50 60", back.Value);
        var widths = ((string)back.Attribute("width")!).Split(" ").Select(s => double.Parse(s, System.Globalization.CultureInfo.InvariantCulture)).ToArray();
        Assert.Equal(new[] { 2, 1.5, 2.5, 3 }, widths.Select(w => Math.Round(w, 2)));
    }

    [Fact]
    public void HighlighterKeepsXournalAlpha()
    {
        var e = XElement.Parse("""<stroke tool="highlighter" color="#ffff007f" width="8.5">1 2 3 4</stroke>""");
        var back = StrokeConverter.ToXml(StrokeConverter.FromXml(e)!);
        Assert.Equal("highlighter", (string?)back.Attribute("tool"));
        Assert.Equal("#ffff007f", (string?)back.Attribute("color"));
        Assert.Equal("8.5", (string?)back.Attribute("width"));
    }

    // Whitespace-only text nodes depend on formatting, not content.
    private static XElement Normalize(XElement e)
    {
        var copy = new XElement(e);
        foreach (var text in copy.DescendantNodes().OfType<XText>().Where(t => string.IsNullOrWhiteSpace(t.Value)).ToList())
        {
            text.Remove();
        }
        return copy;
    }
}
