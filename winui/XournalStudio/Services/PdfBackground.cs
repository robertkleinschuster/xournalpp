using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Data.Pdf;
using Windows.Storage;
using Windows.Storage.Streams;

namespace XournalStudio.Services;

/// <summary>Renders pages of the background PDF with the Windows PDF renderer (Windows.Data.Pdf).</summary>
public sealed class PdfBackground
{
    private readonly PdfDocument pdf;
    private readonly SemaphoreSlim renderLock = new(1, 1);

    private PdfBackground(PdfDocument pdf)
    {
        this.pdf = pdf;
    }

    public int PageCount => (int)pdf.PageCount;

    public static async Task<PdfBackground> LoadAsync(string path)
    {
        var file = await StorageFile.GetFileFromPathAsync(Path.GetFullPath(path));
        return new PdfBackground(await PdfDocument.LoadFromFileAsync(file));
    }

    /// <summary>Page sizes in points (1/72"); the PDF API reports DIPs (1/96").</summary>
    public IReadOnlyList<(double Width, double Height)> GetPageSizes()
    {
        var sizes = new List<(double, double)>();
        for (uint i = 0; i < pdf.PageCount; i++)
        {
            using var page = pdf.GetPage(i);
            sizes.Add((page.Size.Width * 72 / 96, page.Size.Height * 72 / 96));
        }
        return sizes;
    }

    /// <summary>Renders a page into a bitmap that is <paramref name="pixelWidth"/> pixels wide. UI thread only.</summary>
    public async Task<BitmapImage?> RenderAsync(int pageIndex, uint pixelWidth)
    {
        if (pageIndex < 0 || pageIndex >= PageCount)
        {
            return null;
        }

        await renderLock.WaitAsync();
        try
        {
            using var page = pdf.GetPage((uint)pageIndex);
            using var stream = new InMemoryRandomAccessStream();
            await page.RenderToStreamAsync(stream, new PdfPageRenderOptions { DestinationWidth = pixelWidth });
            stream.Seek(0);
            var bitmap = new BitmapImage();
            await bitmap.SetSourceAsync(stream);
            return bitmap;
        }
        finally
        {
            renderLock.Release();
        }
    }
}
