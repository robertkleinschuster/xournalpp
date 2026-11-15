using System.Globalization;
using System.Numerics;
using System.Text;
using System.Xml.Linq;
using Windows.Foundation;
using Windows.UI;
using Windows.UI.Input.Inking;

namespace XournalStudio.Model;

/// <summary>
/// Converts between Xournal's &lt;stroke&gt; elements and Windows ink strokes.
/// Page coordinates are Xournal points (1/72"), used 1:1 as XAML DIPs of the page's InkCanvas.
/// </summary>
public static class StrokeConverter
{
    // Xournal stores per-segment widths (tool width * pressure). Windows ink draws a stroke of size s at
    // pressure p about s * (0.25 + 1.5 p) wide (measured), i.e. exactly s at p = 0.5.
    private static double WidthFactor(double pressure) => 0.25 + 1.5 * pressure;

    private static double PressureFor(double widthFactor) => (widthFactor - 0.25) / 1.5;

    private const byte HighlighterAlpha = 0x7f;

    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    private static readonly InkStrokeBuilder Builder = new();

    public static InkStroke? FromXml(XElement e)
    {
        var coords = ParseNumbers(e.Value);
        int count = coords.Count / 2;
        if (count == 0)
        {
            return null;
        }

        var widths = ParseNumbers((string?)e.Attribute("width") ?? "1");
        double width = widths.Count > 0 && widths[0] > 0 ? widths[0] : 1;
        bool hasPressure = widths.Count > 1;
        bool highlighter = (string?)e.Attribute("tool") == "highlighter";

        var color = XoppColor.Parse((string?)e.Attribute("color"), Microsoft.UI.Colors.Black);
        color.A = 255;

        var attributes = new InkDrawingAttributes
        {
            Color = color,
            Size = new Size(width, width),
            PenTip = PenTipShape.Circle,
            FitToCurve = true,
            IgnorePressure = !hasPressure,
            DrawAsHighlighter = highlighter,
        };

        var points = new List<InkPoint>(count + 1);
        for (int i = 0; i < count; i++)
        {
            float pressure = 0.5f;
            if (hasPressure)
            {
                // widths[i + 1] is the width of segment i; the last point has none.
                double w = widths[Math.Min(i + 1, widths.Count - 1)];
                pressure = (float)Math.Clamp(PressureFor(w / width), 0.01, 1.0);
            }
            points.Add(new InkPoint(new Point(coords[2 * i], coords[2 * i + 1]), pressure));
        }
        if (count == 1)
        {
            // A dot: ink needs at least two points.
            points.Add(points[0]);
        }

        Builder.SetDefaultDrawingAttributes(attributes);
        var stroke = Builder.CreateStrokeFromInkPoints(points, Matrix3x2.Identity);
        stroke.DrawingAttributes = attributes;
        return stroke;
    }

    public static XElement ToXml(InkStroke stroke)
    {
        var attributes = stroke.DrawingAttributes;
        var transform = stroke.PointTransform;
        var points = stroke.GetInkPoints();
        double width = attributes.Size.Width;

        var coords = new StringBuilder(points.Count * 16);
        foreach (var p in points)
        {
            var pos = Vector2.Transform(new Vector2((float)p.Position.X, (float)p.Position.Y), transform);
            if (coords.Length > 0)
            {
                coords.Append(' ');
            }
            coords.Append(Format(pos.X)).Append(' ').Append(Format(pos.Y));
        }

        var widthValue = new StringBuilder(Format(width));
        if (!attributes.IgnorePressure)
        {
            for (int i = 0; i < points.Count - 1; i++)
            {
                widthValue.Append(' ').Append(Format(width * WidthFactor(points[i].Pressure)));
            }
        }

        bool highlighter = attributes.DrawAsHighlighter;
        var color = attributes.Color;
        color.A = highlighter ? HighlighterAlpha : (byte)255;

        return new XElement("stroke",
            new XAttribute("tool", highlighter ? "highlighter" : "pen"),
            new XAttribute("color", XoppColor.Format(color)),
            new XAttribute("width", widthValue.ToString()),
            coords.ToString());
    }

    private static string Format(double v) => v.ToString("0.####", Inv);

    private static List<double> ParseNumbers(string s)
    {
        var result = new List<double>();
        foreach (var part in s.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
        {
            if (double.TryParse(part, NumberStyles.Float, Inv, out double v))
            {
                result.Add(v);
            }
        }
        return result;
    }
}

public static class XoppColor
{
    /// <summary>Parses "#rrggbbaa" (Xournal) or a few named colors of old Xournal files.</summary>
    public static Color Parse(string? s, Color fallback)
    {
        if (string.IsNullOrEmpty(s))
        {
            return fallback;
        }
        if (s[0] == '#' && (s.Length == 9 || s.Length == 7) &&
            uint.TryParse(s.AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint v))
        {
            if (s.Length == 7)
            {
                v = (v << 8) | 0xff;
            }
            return Color.FromArgb((byte)v, (byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8));
        }
        return s switch
        {
            "black" => Color.FromArgb(255, 0, 0, 0),
            "blue" => Color.FromArgb(255, 0x33, 0x33, 0xcc),
            "red" => Color.FromArgb(255, 0xff, 0, 0),
            "green" => Color.FromArgb(255, 0, 0x80, 0),
            "gray" => Color.FromArgb(255, 0x80, 0x80, 0x80),
            "lightblue" => Color.FromArgb(255, 0, 0xc0, 0xff),
            "lightgreen" => Color.FromArgb(255, 0, 0xff, 0),
            "magenta" => Color.FromArgb(255, 0xff, 0, 0xff),
            "orange" => Color.FromArgb(255, 0xff, 0x80, 0),
            "yellow" => Color.FromArgb(255, 0xff, 0xff, 0),
            "white" => Color.FromArgb(255, 0xff, 0xff, 0xff),
            _ => fallback,
        };
    }

    public static string Format(Color c) => $"#{c.R:x2}{c.G:x2}{c.B:x2}{c.A:x2}";
}
