using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace MaaBuilder.Models;

public record Package
{
    [JsonPropertyName("name_template")]
    public string NameTemplate { get; init; }

    [JsonPropertyName("type")]
    [JsonConverter(typeof(JsonStringEnumConverter))]
    public PackageTypes PackageType { get; init; }

    [JsonExtensionData]
    public Dictionary<string, object> Configuration { get; init; }
}
