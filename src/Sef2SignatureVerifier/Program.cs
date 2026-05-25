using System.Reflection;
using System.Security.Cryptography;
using System.Security.Cryptography.Xml;
using System.Xml;

const string PublicKeyResourceName = "Sef2SigningPublicKey.xml";
const string PublicKeySource = "embedded SEF2 signing public key";

if (args.Length != 1)
{
    WriteResult("NotAvailable", exception: "Expected one SEF/SEF2 path argument.");
    return 2;
}

try
{
    var sef2Path = args[0];
    if (!File.Exists(sef2Path))
    {
        WriteResult("NotAvailable", exception: "SEF/SEF2 file was not found.");
        return 0;
    }

    var document = new XmlDocument
    {
        PreserveWhitespace = false
    };
    document.Load(sef2Path);

    var signatureElement = FindSignatureElement(document);
    if (signatureElement is null)
    {
        WriteResult("MissingSignature");
        return 0;
    }

    var signatureMethod = FindAlgorithm(signatureElement, "SignatureMethod");
    var digestMethod = FindAlgorithm(signatureElement, "DigestMethod");
    using var publicKey = LoadPublicKey();
    var signedXml = new SignedXml(document);
    signedXml.LoadXml(signatureElement);
    var valid = signedXml.CheckSignature(publicKey);

    WriteResult(valid ? "Valid" : "Invalid", signatureMethod, digestMethod);
    return 0;
}
catch (Exception ex)
{
    WriteResult("Error", exception: ex.GetType().Name + ": " + ex.Message);
    return 0;
}

static XmlElement? FindSignatureElement(XmlDocument document)
{
    var namespaceManager = new XmlNamespaceManager(document.NameTable);
    namespaceManager.AddNamespace("ds", SignedXml.XmlDsigNamespaceUrl);
    return document.SelectSingleNode("//ds:Signature", namespaceManager) as XmlElement;
}

static string FindAlgorithm(XmlElement signatureElement, string elementName)
{
    var namespaceManager = new XmlNamespaceManager(signatureElement.OwnerDocument.NameTable);
    namespaceManager.AddNamespace("ds", SignedXml.XmlDsigNamespaceUrl);
    return (signatureElement.SelectSingleNode(".//ds:" + elementName, namespaceManager) as XmlElement)
        ?.GetAttribute("Algorithm") ?? "";
}

static RSA LoadPublicKey()
{
    using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(PublicKeyResourceName)
        ?? throw new InvalidOperationException("Public key resource was not found.");
    var document = new XmlDocument
    {
        PreserveWhitespace = false
    };
    document.Load(stream);
    var modulus = Convert.FromBase64String(document.DocumentElement?.SelectSingleNode("Modulus")?.InnerText ?? "");
    var exponent = Convert.FromBase64String(document.DocumentElement?.SelectSingleNode("Exponent")?.InnerText ?? "");

    var rsa = RSA.Create();
    rsa.ImportParameters(new RSAParameters
    {
        Modulus = modulus,
        Exponent = exponent
    });
    return rsa;
}

static void WriteResult(
    string status,
    string signatureMethod = "",
    string digestMethod = "",
    string exception = "")
{
    Console.WriteLine("Status=" + Escape(status));
    Console.WriteLine("SignatureMethod=" + Escape(signatureMethod));
    Console.WriteLine("DigestMethod=" + Escape(digestMethod));
    Console.WriteLine("PublicKeySource=" + Escape(PublicKeySource));
    Console.WriteLine("Exception=" + Escape(exception));
    Console.WriteLine("PreserveWhitespace=false");
}

static string Escape(string value)
{
    return value
        .Replace("\\", "\\\\", StringComparison.Ordinal)
        .Replace("\r", "\\r", StringComparison.Ordinal)
        .Replace("\n", "\\n", StringComparison.Ordinal);
}
