import { mkdirSync, rmSync } from "node:fs";
import { dirname, resolve } from "node:path";
import fs from "node:fs/promises";

async function fallbackDownload(url: string, outPath: string) {
    const response = await fetch(url, {
        headers: { "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)" }
    });
    if (!response.ok) throw new Error(`Failed to download: ${response.statusText}`);

    await Bun.write(outPath, response);
    console.log(`Successfully saved to: ${outPath}`);
}

export default async function download(url: string, outPath: string, connections: number = 4) {
    outPath = resolve(outPath);
    const dirPath = dirname(outPath);
    rmSync(dirPath, { recursive: true, force: true });
    mkdirSync(dirPath, { recursive: true });

    const baseHeaders = {
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        "Accept": "*/*"
    };

    const headResponse = await fetch(url, { method: "HEAD", headers: baseHeaders });
    const contentLength = headResponse.headers.get("content-length");
    const acceptRanges = headResponse.headers.get("accept-ranges");

    if (!contentLength || (acceptRanges !== "bytes" && headResponse.status !== 206)) {
        console.warn("Multi-connection or Range headers not supported. Falling back to single-stream.");
        return fallbackDownload(url, outPath);
    }

    const totalBytes = parseInt(contentLength, 10);
    const chunkSize = Math.ceil(totalBytes / connections);

    console.log(`Total size: ${(totalBytes / 1024 / 1024).toFixed(2)} MB`);

    console.log("Pre-allocating disk space...");
    const fileHandle = await fs.open(outPath, "w+");
    await fileHandle.truncate(totalBytes);

    console.log(`Downloading in ${connections} chunks...`);

    let totalDownloaded = 0;
    const startTime = Date.now();

    const progressInterval = setInterval(() => {
        const elapsedSeconds = (Date.now() - startTime) / 1000;
        const percent = ((totalDownloaded / totalBytes) * 100).toFixed(1);
        const speedBytesPerSec = elapsedSeconds > 0 ? totalDownloaded / elapsedSeconds : 0;
        const speedMB = (speedBytesPerSec / 1024 / 1024).toFixed(2);

        const barLength = 30;
        const filledLength = Math.round((barLength * totalDownloaded) / totalBytes);
        const bar = "█".repeat(filledLength) + "-".repeat(barLength - filledLength);

        const downloadedMB = (totalDownloaded / 1024 / 1024).toFixed(2);
        const totalMB = (totalBytes / 1024 / 1024).toFixed(2);

        process.stdout.write(
            `\r[${bar}] ${percent}% | ${downloadedMB}/${totalMB} MB | ${speedMB} MB/s`
        );
    }, 200);

    try {
        const downloadChunks = Array.from({ length: connections }, async (_, i) => {
            const start = i * chunkSize;
            const end = Math.min(start + chunkSize - 1, totalBytes - 1);

            const response = await fetch(url, {
                headers: {
                    ...baseHeaders,
                    Range: `bytes=${start}-${end}`
                },
            });

            if (!response.ok) {
                throw new Error(`Failed to download chunk ${i}: ${response.statusText}`);
            }

            const reader = response.body?.getReader();
            if (!reader) throw new Error(`Failed to get stream reader for chunk ${i}`);

            let writeOffset = start;

            while (true) {
                const { done, value } = await reader.read();
                if (done) break;

                await fileHandle.write(value, 0, value.length, writeOffset);

                writeOffset += value.length;
                totalDownloaded += value.length;
            }
        });

        await Promise.all(downloadChunks);

        clearInterval(progressInterval);
        const totalMB = (totalBytes / 1024 / 1024).toFixed(2);
        process.stdout.write(`\r[${"█".repeat(30)}] 100.0% | ${totalMB}/${totalMB} MB | Finished\n`);

    } catch (error) {
        clearInterval(progressInterval);
        process.stdout.write(`\n`);
        throw error;
    } finally {
        await fileHandle.close();
    }

    console.log(`Successfully saved to: ${outPath}`);
}