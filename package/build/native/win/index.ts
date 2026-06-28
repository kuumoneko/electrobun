import { join, resolve } from 'node:path';
import { getArch, isNewer } from '../../../share/os';
import runMsvcCommand from '../../../share/runmsvc';
import { existsSync, readdirSync, renameSync } from 'node:fs';
import download from '../../../share/download';
import { $ } from 'bun';

async function getNuget(path: string): Promise<string> {
    path = resolve(path);

    const nugetPath = join(path, "vendors", "nuget", "nuget.exe")
    if (!existsSync(nugetPath)) {
        const downloadedUrl = "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe";
        await download(downloadedUrl, nugetPath);
    }

    return nugetPath;
}

async function getWebview2Include(path: string) {
    path = resolve(path);

    const webview2Include = join(path, 'vendors', 'webview2', 'Microsoft.Web.WebView2', 'build', 'native', 'include');
    const webview2Lib = join(path, 'vendors', 'webviews2', 'Microsoft.Web.WebView2', 'build', 'native', getArch(), 'WebView2LoaderStatic.lib');

    if (!existsSync(webview2Include) || !existsSync(webview2Lib)) {
        const nutget = await getNuget(path);
        const webview2Dir = join(path, 'vendors', 'webview2')
        await $`${nutget} install Microsoft.Web.WebView2 -OutputDirectory ${webview2Dir} -Source https://api.nuget.org/v3/index.json`;

        const downloadedwebview2Dir = readdirSync(webview2Dir).find((dir: string) => dir.startsWith("Microsoft.Web.WebView2"));
        if (downloadedwebview2Dir && webview2Dir && webview2Dir !== "Microsoft.Web.WebView2") {
            renameSync(join(webview2Dir, downloadedwebview2Dir), join(webview2Dir, "Microsoft.Web.WebView2"));
        }
    }
    return {
        webview2Include, webview2Lib
    }
}

export default async function build_win_native(path: string) {
    path = resolve(path);

    const nativeSource = join(path, "src", "native", "win", "nativeWrapper.cpp");
    const nativeDll = join(path, "src", "native", "win", "build", "libNativeWrapper.dll");
    const nativeLib = join(path, "src", "native", "win", "build", "libNativeWrapper.dll");
    const nativeObj = join(path, "src", "native", "win", "build", "libNativeWrapper.obj");

    console.log("\n" + Array(10).fill("=").join("") + ` Build Windows native wrapper ` + Array(10).fill("=").join(""))
    if (isNewer(nativeSource, [nativeLib, nativeObj, nativeDll])) {
        console.log("Windows native wrapper is changed! Building...");
        const { webview2Include, webview2Lib } = await getWebview2Include(path);
        await runMsvcCommand(`cl /c /EHsc /std:c++20 /DNOMINMAX /MT /I"${webview2Include}" /D_USRDLL /D_WINDLL /Fo"${nativeObj}" "${nativeSource}"`);
        await runMsvcCommand(`link /DLL /OUT:"${nativeDll}" user32.lib ole32.lib shell32.lib shlwapi.lib advapi32.lib dcomp.lib d2d1.lib kernel32.lib comctl32.lib "${webview2Lib}" libcmt.lib /IMPLIB:"${nativeLib}" "${nativeObj}"`);

        console.log(`Windows native wrapper build done.`)
    }
    else {
        console.log("Windows native wrapper is unchanged! Skipping...");
    }
}