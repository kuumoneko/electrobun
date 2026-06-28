import { checkPackage, installPackage, isNewer } from "../../../share/os";
import { join, resolve } from "node:path"
import { spawnSync } from 'bun';
import { existsSync, mkdirSync } from "node:fs";

const requiredPackages = [
    "libdbus-1-dev",
    "pkg-config",
    "build-essential"
]

/**
 * 
 * @param path 
 * path points to the directory that has filedialog.cpp source code
 */
export default async function build_linux_filedialog(path: string) {
    const missingDeps = await checkPackage(requiredPackages);
    if (missingDeps.length > 0) {
        await installPackage(missingDeps);
    }

    path = resolve(path);
    const sourcePath = join(path, "filedialog.cpp");
    const libSource = join(path, "tinyfiledialogs", "tinyfiledialogs.c")
    if (!existsSync(sourcePath)) throw new Error(`filedialog.cpp not found at ${sourcePath}`);
    if (!existsSync(libSource)) throw new Error(`tinyfiledialogs.c not found at ${libSource}`);

    const buildDir = join(path, "build");
    const outPath = join(buildDir, "filedialog.so");

    mkdirSync(buildDir, { recursive: true });

    if (isNewer(sourcePath, [outPath])) {
        spawnSync([
            "g++",
            "-shared",
            "-fPIC",
            "-o", outPath,
            "-I.", sourcePath, libSource,
            "-lX11", "-O2", "-D_GNU_SOURCE"],
            {
                stdout: "ignore",
                stderr: "ignore"
            });

        spawnSync(["strip", outPath], {
            stdout: "ignore",
            stderr: "ignore"
        });
    }
    else {
        console.log("filedialog is unchanged, Skipping...")
    }
}