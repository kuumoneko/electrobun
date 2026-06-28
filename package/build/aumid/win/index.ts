import { existsSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { isNewer } from '../../../share/os';
import runMsvcCommand from '../../../share/runmsvc';

/**
 * 
 * @param path 
 * path points to the directory that has audmid.cpp source code
 */
export default async function build_win_aumid(path: string) {
    path = resolve(path);
    const sourcePath = join(path, "aumid.cpp");
    if (!existsSync(sourcePath)) throw new Error(`aumid.cpp not found at ${sourcePath}`);
    const objPath = join(path, "build", "aumid.obj");
    const libPath = join(path, "build", "aumid.lib");
    const dllPath = join(path, "build", "aumid.dll");
    if (isNewer(sourcePath, [objPath, libPath, dllPath])) {
        console.log("aumid.cpp is changed! Building...");
        try {
            await runMsvcCommand(`cl /c /EHsc /std:c++20 /MT /D_USRDLL /D_WINDLL /Fo"${objPath}" "${sourcePath}"`);
            await runMsvcCommand(`link /DLL /OUT:"${dllPath}" user32.lib ole32.lib oleaut32.lib shell32.lib kernel32.lib /IMPLIB:"${libPath}" "${objPath}"`);
        } catch (e) { console.error(e); }
    }
}