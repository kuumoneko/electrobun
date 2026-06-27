import { existsSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { isNewer } from '../../utils/os';
import runMsvcCommand from '../../utils/runmsvc';

/**
 * 
 * @param path 
 * path points to the directory that has filedialog.cpp source code
 */
export default async function build_win_filedialog(path: string) {
    path = resolve(path);
    const sourcePath = join(path, "filedialog.cpp");
    if (!existsSync(sourcePath)) throw new Error(`filedialog.cpp not found at ${sourcePath}`);
    const objPath = join(path, "build", "filedialog.obj");
    const libPath = join(path, "build", "filedialog.lib");
    const dllPath = join(path, "build", "filedialog.dll");

    if (isNewer(sourcePath, [objPath, libPath, dllPath])) {
        console.log("filedialog.cpp is changed! Building...");
        try {
            await runMsvcCommand(`cl /c /EHsc /std:c++20 /MT /D_USRDLL /D_WINDLL /Fo"${objPath}" "${sourcePath}"`);
            await runMsvcCommand(`link /DLL /OUT:"${dllPath}" user32.lib ole32.lib oleaut32.lib shell32.lib kernel32.lib /IMPLIB:"${libPath}" "${objPath}"`);
        } catch (e) { console.error(e); }
    }
    else {
        console.log("filedialog is unchanged, Skipping...")
    }
}