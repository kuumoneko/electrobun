import { BrowserWindow } from "./core/BrowserWindow";
import { BrowserView } from "./core/BrowserView";
import { Tray } from "./core/Tray";
import { Updater, } from "./core/Updater";
import * as Utils from "./core/Utils";
import { type RPCSchema, createRPC, } from "../shared/rpc.js";
import type { ElectrobunConfig } from "./ElectrobunConfig";
import { Screen, } from "./proc/native";
import type { MenuItemConfig, } from "./proc/native";

// Named Exports
export {
	type RPCSchema,
	type ElectrobunConfig,
	type MenuItemConfig,
	createRPC,
	BrowserWindow,
	BrowserView,
	Tray,
	Updater,
	Utils,
	Screen,
};