import { getOS } from "./os";
import { $ } from 'bun';
import { existsSync, mkdirSync } from 'node:fs';

async function check7zip(): Promise<string> {
    const OS = getOS();
    try {
        const zipPath = (await $`where 7z`.quiet()).stdout.toString().trim();
        return zipPath;
    } catch {
        if (OS === "win") {
            const wingetPath = (await $`where winget`.quiet()).stdout.toString().trim();
            // const psCommand = "winget install 7zip.7zip --silent --accept-source-agreements --accept-package-agreements";
            // await Bun.spawn(
            //     ["powershell", "-Command", `Start-Process powershell -ArgumentList '-NoProfile -ExecutionPolicy Bypass -Command & { ${psCommand} }' -Verb RunAs`],
            //     {
            //         stderr: "ignore", stdout: "ignore"
            //     }
            // ).exited;
            await $`${wingetPath} install 7zip.7zip --silent --accept-source-agreements --accept-package-agreements`.quiet();
        }
        else if (OS === "linux") {
            await $`sudo apt install -y p7zip-full`.quiet();
        }
        else if (OS === "macos") {
            await $`brew install p7zip`.quiet();
        }
        return ""
    }
}

export async function decompress(source: string, out: string) {
    const zipPath = await check7zip();

    try {
        await $`cmd.exe /c ${zipPath}`.quiet();
    } catch { throw new Error("7zip is not installed"); }

    mkdirSync(out, { recursive: true });

    await $`cmd.exe /c ${zipPath} x ${source} -o${out}`.quiet();
    console.log(`Compressed to ${out}.`)
}