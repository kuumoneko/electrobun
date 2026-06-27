import { existsSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { isNewer } from '../../utils/os';
import runMsvcCommand from '../../utils/runmsvc';

/**
 * 
 * @param path 
 * path points to the directory that has smtc.cpp source code
 */
export default async function build_win_smtc(path: string) {
    path = resolve(path);
    const sourcePath = join(path, "smtc.cpp");
    if (!existsSync(sourcePath)) throw new Error(`smtc.cpp not found at ${sourcePath}`);
    const objPath = join(path, "build", "smtc.obj");
    const libPath = join(path, "build", "smtc.lib");
    const dllPath = join(path, "build", "smtc.dll");
    if (isNewer(sourcePath, [objPath, libPath, dllPath])) {
        console.log("smtc.cpp is changed! Building...");
        try {
            await runMsvcCommand(`cl /c /EHsc /std:c++20 /MT /D_USRDLL /D_WINDLL /Fo"${objPath}" "${sourcePath}"`);
            await runMsvcCommand(`link /DLL /OUT:"${dllPath}" user32.lib ole32.lib oleaut32.lib shell32.lib kernel32.lib runtimeobject.lib mfplat.lib mf.lib /IMPLIB:"${libPath}" "${objPath}"`);
        } catch (e) { console.error(e); }
    }
}