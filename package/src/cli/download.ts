import { spawnSync } from "bun";
import { mkdir } from "node:fs/promises";
import { dirname, resolve } from "node:path";

/**
 * Draws a real-time progress bar in the terminal.
 */
function drawProgressBar(current: number, total: number) {
    const percentage = total === 0 ? 0 : current / total;
    const barLength = 40;
    const filled = Math.round(barLength * percentage);
    const empty = barLength - filled;
    const bar = "█".repeat(filled) + "-".repeat(empty);
    const percentStr = (percentage * 100).toFixed(2);

    process.stdout.write(`\r[2/3] Downloading: [${bar}] ${percentStr}% (${current}/${total} bytes)`);
}

/**
 * Downloads a tar/tar.gz file via multiple connections and extracts it.
 */
export default async function downloadAndExtract(
    url: string,
    cacheFilePath: string,
    outfolderPath: string,
    connections: number = 4
) {
    console.log(`Fetching file metadata from: ${url}`);

    const headResponse = await fetch(url);
    if (!headResponse.ok) {
        throw new Error(`Failed to fetch headers: ${headResponse.statusText}`);
    }

    const contentLength = headResponse.headers.get("content-length");
    const acceptRanges = headResponse.headers.get("accept-ranges") === "bytes";
    const totalSize = contentLength ? parseInt(contentLength, 10) : 0;

    await mkdir(dirname(cacheFilePath), { recursive: true });

    let finalBuffer: Uint8Array;

    if (totalSize > 0 && acceptRanges && connections > 1) {
        const chunkSize = Math.ceil(totalSize / connections);
        const downloadPromises: Promise<{ start: number; data: Uint8Array }>[] = [];
        let downloadedBytes = 0;

        // Pre-allocate the full buffer to avoid memory fragmentation
        finalBuffer = new Uint8Array(totalSize);
        drawProgressBar(0, totalSize);

        for (let i = 0; i < connections; i++) {
            const start = i * chunkSize;
            const end = i === connections - 1 ? totalSize - 1 : start + chunkSize - 1;

            const chunkPromise = fetch(url, {
                headers: { Range: `bytes=${start}-${end}` },
            }).then(async (res) => {
                if (!res.ok || !res.body) throw new Error(`Chunk ${i} failed`);

                const reader = res.body.getReader();
                let offset = start;

                while (true) {
                    const { done, value } = await reader.read();
                    if (done) break;
                    if (value) {
                        finalBuffer.set(value, offset);
                        offset += value.length;
                        downloadedBytes += value.length;
                        drawProgressBar(downloadedBytes, totalSize);
                    }
                }
                return { start, data: finalBuffer.subarray(start, offset) };
            });

            downloadPromises.push(chunkPromise);
        }

        await Promise.all(downloadPromises);
        process.stdout.write("\n");
    } else {
        console.log(`Server does not support Range requests. Downloading via single connection...`);
        const response = await fetch(url);
        if (!response.ok) throw new Error("Download failed");

        // Convert response directly to ArrayBuffer/Uint8Array
        const arrayBuffer = await response.arrayBuffer();
        finalBuffer = new Uint8Array(arrayBuffer);
    }

    // Use Bun.write() - this automatically handles opening, writing, and closing
    // We await it to ensure the OS has finished the write operation
    await Bun.write(cacheFilePath, finalBuffer);
    console.log(`\nFile saved to cache: ${cacheFilePath}`);

    console.log(`Extracting to ${outfolderPath}...`);

    try {
        // We use the buffer already in memory for extraction to be even faster, 
        // but if you want to be safe and read from the disk:
        try {
            if (cacheFilePath.endsWith(".zip")) {
                // 3. Extract the downloaded file natively
                console.log(`Extracting to: ${outfolderPath}`);
                spawnSync(["powershell", `Expand-Archive -Path ${resolve(cacheFilePath)} -DestinationPath ${resolve(outfolderPath)} -Force`], {
                    stdout: "inherit", stderr: "inherit"
                });
                console.log(`Extracted entries to ${outfolderPath}.`);
            }
            else {
                console.log(`Extracting to: ${outfolderPath}`);

            }
        } catch (error) {
            console.error("❌ Extraction failed. Ensure the downloaded file is a valid .tar or .tar.gz archive.");
            throw error;
        }
    } catch (error) {
        console.error(error);
    }
}