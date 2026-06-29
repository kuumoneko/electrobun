import { which, $ } from "bun";

async function ensure7zip(): Promise<string> {
    let zipPath = (await $`where 7z`.quiet()).stdout.toString().trim();
    if (zipPath) return zipPath;

    const wingetPath = (await $`where 7z`.quiet()).stdout.toString().trim()
    if (wingetPath) {
        await $`cmd /c ${wingetPath} install 7zip.7zip --silent --accept-source-agreements --accept-package-agreements`.quiet();
    }

    zipPath = (await $`where 7z`.quiet()).stdout.toString().trim();
    if (!zipPath) throw new Error("7zip is not installed");
    return zipPath;
}

const sevenZipPath = await ensure7zip();

const source = "E:\\Download\\Compressed\\zig-windows-x86_64-0.13.0.zip"
await $`cmd /c ${sevenZipPath} -scrcSHA256 h ${source}`.quiet().then(value => {
    const text = value.stdout.toString().split("\n");
    console.log(text.find(value => value.includes("SHA256 for data"))?.split("SHA256 for data:")[1]?.trim())
})