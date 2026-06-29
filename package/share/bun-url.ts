export function getBunDownloadInfo(
    os: "win" | "macos" | "linux",
    arch: "x64" | "arm64",
): { urlSegment: string; dirName: string } {
    if (os === "win") {
        return { urlSegment: "bun-windows-x64.zip", dirName: "bun-windows-x64" };
    } else if (os === "macos") {
        const suffix = arch === "arm64" ? "aarch64" : "x64";
        return { urlSegment: `bun-darwin-${suffix}.zip`, dirName: `bun-darwin-${suffix}` };
    } else {
        const suffix = arch === "arm64" ? "aarch64" : "x64";
        return { urlSegment: `bun-linux-${suffix}.zip`, dirName: `bun-linux-${suffix}` };
    }
}
