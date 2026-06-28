import { join, resolve } from 'node:path';
import { checkPackage, installPackage, isNewer } from '../../../share/os';
import { $ } from 'bun';
import { existsSync, mkdirSync } from 'node:fs';

const requiredPackages = [
    "libwebkit2gtk-4.0-dev", "libgtk-3-0", "libayatana-appindicator3-0.1-cil-dev"
]

export default async function build_linux_native(path: string) {
    path = resolve(path);

    const missingPackages = await checkPackage(requiredPackages);
    if (missingPackages.length > 0) await installPackage(missingPackages);

    let pkgConfigCflags = "";
    let pkgConfigLibs = "";
    let hasAppIndicator = false;

    try {
        const cflagsResult = await $`pkg-config --cflags webkit2gtk-4.0 gtk+-3.0 ayatana-appindicator3-0.1`.quiet();
        pkgConfigCflags = cflagsResult.stdout.toString().trim();
        const libsResult = await $`pkg-config --libs webkit2gtk-4.0 gtk+-3.0 ayatana-appindicator3-0.1`.quiet();
        pkgConfigLibs = libsResult.stdout.toString().trim();
        hasAppIndicator = true;
    } catch {
        throw new Error("Missing deps, try to install and rerun.")
    }

    const nativeDir = join(path, 'src', 'native');
    const nativeSource = join(nativeDir, 'linux', 'nativeWrapper.cpp');

    if (!existsSync(nativeSource)) {
        throw new Error(`Native wrapper.cpp not found at ${nativeSource}`);
    }

    const outDir = join(nativeDir, 'linux', 'build');
    mkdirSync(outDir, { recursive: true });

    const nativeObj = join(outDir, 'nativeWrapper.o');
    const nativeLib = join(outDir, 'nativeWrapper.so')

    console.log("\n" + Array(10).fill("=").join("") + ` Build Linux Native Wrapper ` + Array(10).fill("=").join(""))

    if (isNewer(nativeSource, [nativeLib, nativeObj])) {
        console.log("Nativewrapper is changed! Building...");

        const cflagsParts = pkgConfigCflags.split(/\s+/).filter((f) => f);

        const compileCmd = [
            "g++", "-c", "-std=c++20", "-fPIC",
            ...cflagsParts,
            ...(hasAppIndicator ? [] : ["-DNO_APPINDICATOR"]),
            "-o", nativeObj,
            nativeSource,
        ];

        await $`${compileCmd.join(" ")}`;


        const libsParts = pkgConfigLibs.split(/\s+/).filter((f) => f);
        const linkCmd = [
            "g++", "-shared",
            "-o", nativeLib,
            nativeObj,
            ...libsParts,
            "-ldl", "-lpthread",
        ];
        await $`${linkCmd.join(" ")}`;
        console.log(`Nativewrapper build done.`)
    }
    else {
        console.log("Nativewrapper is unchanged! Skipping...");
    }
}