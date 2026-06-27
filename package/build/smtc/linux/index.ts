import { checkPackage, installPackage, isNewer } from "../../utils/os";
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
 * path points to the directory that has smtc.cpp source code
 */
export default async function build_linux_smtc(path: string) {
    const missingDeps = await checkPackage(requiredPackages);
    if (missingDeps.length > 0) {
        await installPackage(missingDeps);
    }

    path = resolve(path);
    const sourcePath = join(path, "smtc.cpp");
    if (!existsSync(sourcePath)) throw new Error(`smtc.cpp not found at ${sourcePath}`);

    const buildDir = join(path, "build");
    const outPath = join(buildDir, "smtc.so");

    mkdirSync(buildDir, { recursive: true });

    if (isNewer(sourcePath, [outPath])) {
        const pkgConfig = Bun.spawnSync(["pkg-config", "--cflags", "--libs", "dbus-1"]);
        const flags = pkgConfig.stdout.toString().trim().split(" ");

        spawnSync([
            "g++",
            "-shared",
            "-fPIC",
            "-o", outPath,
            "-I", sourcePath,
            ...flags,
            "-std=c++11", "-O2", "-Wl,-soname,smtc.so"],
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
        console.log("SMTC is unchanged, Skipping...")
    }
}