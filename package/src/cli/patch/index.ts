import { join } from "node:path";
import { getTarballFileName } from "../../shared/naming";
import { decompress, ensure7zip } from "../../share/7z";

export default async function createPatch(options: {
    baseUrl: string;
    generatePatch: boolean;
    appFileName: string;
    platformPrefix: string;
    buildFolder: string;
    bsdiffPath: string;
    tarPath: string;
    os: string;
}): Promise<string | null> {
    if (options.generatePatch === false) {
        console.log("Patch generation disabled (release.generatePatch = false)");
        return null;
    }

    if (!options.baseUrl || options.baseUrl.trim() === "") {
        console.log("No baseUrl configured, skipping patch generation");
        console.log("To enable patch generation, configure baseUrl in your electrobun.config");
        return null;
    }

    const tarballFileName = getTarballFileName(options.appFileName, options.os as any);

    const urlToPrevVersion = await fetch(`${options.baseUrl.replace(/\/+$/, "")}/latest`, {
        headers: {
            'User-Agent': 'node-fetch-example',
            'Accept': 'application/json'
        },
        method: "GET"
    });

    const json = await urlToPrevVersion.json();
    const latestVersion = json.tag_name;

    const urlToPrevUpdateJson = `${options.baseUrl.replace(/\/+$/, "")}/${latestVersion}/${options.platformPrefix}-update.json`;
    const cacheBuster = Math.random().toString(36).substring(7);

    const updateJsonResponse = await fetch(urlToPrevUpdateJson);
    const tempp = await updateJsonResponse.arrayBuffer();
    const updateJson = JSON.parse(Buffer.from(tempp).toString("utf8"))

    const urlToLatestTarball = `${options.baseUrl.replace(/\/+$/, "")}/${latestVersion}/${options.platformPrefix}-${tarballFileName}`;

    if (!updateJsonResponse || !updateJsonResponse.ok) {
        console.log("previous version not found at: ", urlToLatestTarball);
        console.log("skipping diff generation");
        return null;
    }

    const prevHash = updateJson.hash;
    console.log("PREVIOUS HASH", prevHash);

    const prevVersionCompressedTarballPath = join(options.buildFolder, `${prevHash}.tar.zst`);
    let canGeneratePatch = true;

    if (!(await Bun.file(prevVersionCompressedTarballPath).exists())) {
        const response = await fetch(urlToLatestTarball + `?${cacheBuster}`);
        if (response && response.ok && response.body) {
            const reader = response.body.getReader();
            const totalBytesHeader = response.headers.get("content-length");
            const totalBytes = totalBytesHeader ? Number(totalBytesHeader) : undefined;
            let downloadedBytes = 0;
            let lastLogTime = Date.now();
            const logIntervalMs = 5_000;

            const writer = Bun.file(prevVersionCompressedTarballPath).writer();

            while (true) {
                const { done, value } = await reader.read();
                if (done) break;
                downloadedBytes += value.length;
                const now = Date.now();
                if (now - lastLogTime >= logIntervalMs) {
                    if (totalBytes && Number.isFinite(totalBytes)) {
                        const percent = ((downloadedBytes / totalBytes) * 100).toFixed(1);
                        console.log(`Downloading previous version... ${percent}% (${downloadedBytes}/${totalBytes} bytes)`);
                    } else {
                        console.log(`Downloading previous version... ${downloadedBytes} bytes`);
                    }
                    lastLogTime = now;
                }
                await writer.write(value);
            }
            await writer.flush();
            writer.end();
        } else {
            canGeneratePatch = false;
        }
    }

    console.log("decompress prev funn bundle...");
    const prevExtractDir = join(options.buildFolder, "prev-extracted");
    const prevTarballPath = join(options.buildFolder, "prev.tar");

    if (canGeneratePatch && !(await Bun.file(prevTarballPath).exists())) {
        // 7z strips both zstd and tar layers at once, extracting raw files
        await decompress(prevVersionCompressedTarballPath, prevExtractDir);

        // Re-tar the extracted files to match the new tar's structure
        const [zipPath] = await ensure7zip();
        const tarResult = Bun.spawnSync(
            [zipPath, "a", "-ttar", prevTarballPath, "."],
            { cwd: prevExtractDir, stdio: "inherit" },
        );
        if (tarResult.exitCode !== 0) {
            console.log("Failed to re-tar previous bundle, skipping patch generation");
            canGeneratePatch = false;
        }
    }

    if (!canGeneratePatch) {
        console.log("Failed to fetch previous tarball, skipping patch generation");
        return null;
    }

    console.log("diff previous and new tarballs...");
    const patchFilePath = join(options.buildFolder, `${options.appFileName}.patch`);
    const result = Bun.spawnSync(
        [
            options.bsdiffPath,
            prevTarballPath,
            options.tarPath,
            patchFilePath,
            "--use-zstd",
            "--level", "3",
        ],
        {
            cwd: options.buildFolder,
            stdout: "inherit",
            stderr: "pipe",
        },
    );

    if (!result.success) {
        console.error("\n" + "=".repeat(80));
        console.log(result.stderr?.toString());
        console.error("WARNING: Patch generation failed (exit code " + result.exitCode + ")");
        console.error("Delta updates will not be available for this release.");
        console.error("Users will download the full update instead.");
        console.error("=".repeat(80) + "\n");
        return null;
    }

    await Bun.file(prevTarballPath).unlink().catch(console.error);

    return patchFilePath;
}
