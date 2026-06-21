import { ffi, native } from "../proc/native";
import { electrobunEventEmitter } from "../events/eventEmitter";
import { homedir } from "node:os";
import { join } from "node:path";
import { OS } from "../../shared/platform";
import { readFileSync } from "node:fs";

let isQuitting = false;

export const quit = () => {
	if (isQuitting) return;
	isQuitting = true;

	const beforeQuitEvent = electrobunEventEmitter.events.app.beforeQuit({});
	electrobunEventEmitter.emitEvent(beforeQuitEvent);

	if (
		beforeQuitEvent.responseWasSet &&
		beforeQuitEvent.response?.allow === false
	) {
		isQuitting = false;
		return;
	}

	if (native) {
		native.symbols.stopEventLoop();
		native.symbols.waitForShutdownComplete(5000);
		native.symbols.forceExit(0);
	} else {
		process.exit(0);
	}
};

const _originalProcessExit = process.exit;
process.exit = ((code?: number) => {
	if (native) {
		if (isQuitting) {
			native.symbols.forceExit(code ?? 0);
			return;
		}
		quit();
	} else {
		_originalProcessExit(code ?? 0);
	}
}) as typeof process.exit;

export const openFileDialog = async (
	opts: {
		startingFolder?: string;
		allowedFileTypes?: string;
		canChooseFiles?: boolean;
		canChooseDirectory?: boolean;
		allowsMultipleSelection?: boolean;
	} = {},
): Promise<string[]> => {
	const optsWithDefault = {
		...{
			startingFolder: "~/",
			allowedFileTypes: "*",
			canChooseFiles: true,
			canChooseDirectory: true,
			allowsMultipleSelection: true,
		},
		...opts,
	};

	const result = await ffi.request.openFileDialog({
		startingFolder: optsWithDefault.startingFolder,
		allowedFileTypes: optsWithDefault.allowedFileTypes,
		canChooseFiles: optsWithDefault.canChooseFiles,
		canChooseDirectory: optsWithDefault.canChooseDirectory,
		allowsMultipleSelection: optsWithDefault.allowsMultipleSelection,
	});

	const filePaths = result.split(",");
	return filePaths;
};

export type MessageBoxOptions = {
	type?: "info" | "warning" | "error" | "question";
	title?: string;
	message?: string;
	detail?: string;
	buttons?: string[];
	defaultId?: number;
	cancelId?: number;
};

export type MessageBoxResponse = {
	response: number; // Index of the clicked button
};

export const showMessageBox = async (
	opts: MessageBoxOptions = {},
): Promise<MessageBoxResponse> => {
	const {
		type = "info",
		title = "",
		message = "",
		detail = "",
		buttons = ["OK"],
		defaultId = 0,
		cancelId = -1,
	} = opts;

	const response = ffi.request.showMessageBox({
		type,
		title,
		message,
		detail,
		buttons,
		defaultId,
		cancelId,
	});

	return { response };
};
const home = homedir();

let _versionInfo: { identifier: string; channel: string } | undefined;
function getVersionInfo(): { identifier: string; channel: string } {
	if (_versionInfo) return _versionInfo;
	try {
		const resourcesDir = "Resources";
		const raw = readFileSync(join("..", resourcesDir, "version.json"), "utf-8");
		const parsed = JSON.parse(raw);
		_versionInfo = { identifier: parsed.identifier, channel: parsed.channel };
		return _versionInfo;
	} catch (error) {
		console.error("Failed to read version.json", error);
		_versionInfo = { identifier: "", channel: "" };
		return _versionInfo;
	}
}

function getAppDataDir(): string {
	switch (OS) {
		case "macos":
			return join(home, "Library", "Application Support");
		case "win":
			return process.env["LOCALAPPDATA"] || join(home, "AppData", "Local");
		case "linux":
			return process.env["XDG_DATA_HOME"] || join(home, ".local", "share");
	}
}


export const paths = {
	get userData(): string {
		const { identifier, channel } = getVersionInfo();
		return join(getAppDataDir(), identifier, channel);
	},
	get appData(): string {
		return getAppDataDir();
	},
};
