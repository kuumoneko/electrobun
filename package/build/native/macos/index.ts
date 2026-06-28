import { $ } from 'bun';
import { existsSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { isNewer } from '../../../share/os';

export default async function build_macos_native(path: string) {
    path = resolve(path);

    const nativePath = join(path, 'src', 'native');
    const nativeSource = join(path, 'macos', 'nativeWrapper.mm');
    if (!existsSync(nativeSource)) {
        throw new Error(`nativeWrapper.mm not found at ${nativeSource}`);
    }

    const outDir = join(nativePath, 'macos', 'build');

    const nativeObj = join(outDir, 'nativeWrapper.o');
    const nativeLib = join(outDir, 'nativeWarpper.dylib');

    console.log("\n" + Array(10).fill("=").join("") + ` Build Macos Native Wrapper ` + Array(10).fill("=").join(""))
    if (isNewer(nativeSource, [nativeLib, nativeObj])) {
        console.log("Nativewrapper is changed! Building...");
        await $`mkdir -p src/native/macos/build`;
        const compileFlags = [
            "clang++", "-c", nativeSource,
            "-o", nativeObj,
            "-fobjc-arc", "-fno-objc-msgsend-selector-stubs",
            "-std=c++20",
        ];
        await $`${compileFlags}`;

        await $`mkdir -p src/native/build`;
        const linkFlags = [
            "clang++", "-o", nativeLib,
            nativeObj,
            "-framework", "Cocoa", "-framework", "WebKit",
            "-framework", "QuartzCore", "-framework", "Metal",
            "-framework", "MetalKit", "-framework", "UserNotifications",
            "-stdlib=libc++", "-shared",
            "-install_name", "@executable_path/libNativeWrapper.dylib",
            "-Wl,-rpath,@executable_path",
        ];
        await $`${linkFlags}`;

        console.log(`Nativewrapper build done.`)
    }
    else {
        console.log("Nativewrapper is changed! Building...");

    }
}