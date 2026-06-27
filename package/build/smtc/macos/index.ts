import { join, resolve } from "node:path"
import { spawnSync } from 'bun';
import { existsSync, mkdirSync } from "node:fs";
import { isNewer } from "../../utils/os";
/**
 * 
 * @param path 
 * path points to the directory that has smtc.mm source code
 */
export default async function build_macos_smtc(path: string) {
    path = resolve(path);
    const sourcePath = join(path, "smtc.mm");
    if (!existsSync(sourcePath)) throw new Error(`smtc.mm not found at ${sourcePath}`);

    const buildDir = join(path, "build");
    const outPath = join(buildDir, "smtc.dylib");

    mkdirSync(buildDir, { recursive: true });

    if (isNewer(sourcePath, [outPath])) {
        spawnSync([
            "clang++",
            "-shared",
            "-fPIC",
            "-o", outPath,
            "-I", sourcePath,
            "-framework", "MediaPlayer",
            "-framework", "AppKit",
            "-framework", "Cocoa",
            "-std=c++11", "-O2", "-ObjC++"],
            {
                stdout: "ignore",
                stderr: "ignore"
            });

        spawnSync(["strip", "-S", outPath], {
            stdout: "ignore",
            stderr: "ignore"
        });
    }
    else {
        console.log("SMTC is unchanged, Skipping...")
    }
}