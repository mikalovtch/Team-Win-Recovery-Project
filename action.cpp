// image.cpp - GUIImage object

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include <string>
#include <sstream>

extern "C" {
#include "../common.h"
#include "../roots.h"
#include "../tw_reboot.h"
#include "../minui/minui.h"
#include "../recovery_ui.h"
#include "../ddftw.h"
#include "../backstore.h"
#include "../extra-functions.h"

int install_zip_package(const char* zip_path_filename);
void fix_perms();
int erase_volume(const char* path);
void wipe_dalvik_cache(void);
void update_system_details();
int nandroid_back_exe(void);
void set_restore_files(void);
int nandroid_rest_exe(void);
void wipe_data(int confirm);
void wipe_battery_stats(void);
void wipe_rotate_data(void);
int usb_storage_enable(void);
int usb_storage_disable(void);
int __system(const char *command);
void run_script(const char *str1, const char *str2, const char *str3, const char *str4, const char *str5, const char *str6, const char *str7, int request_confirm);
void update_tz_environment_variables();
void install_htc_dumlock(void);
void htc_dumlock_restore_original_boot(void);
void htc_dumlock_reflash_recovery_to_boot(void);
void update_system_details();
};

#include "rapidxml.hpp"
#include "objects.hpp"


void curtainClose(void);

GUIAction::GUIAction(xml_node<>* node)
    : Conditional(node)
{
    xml_node<>* child;
    xml_node<>* actions;
    xml_attribute<>* attr;

    mKey = 0;

    if (!node)  return;

    // First, get the action
    actions = node->first_node("actions");
    if (actions)    child = actions->first_node("action");
    else            child = node->first_node("action");

    if (!child) return;

    while (child)
    {
        Action action;

        attr = child->first_attribute("function");
        if (!attr)  return;
    
        action.mFunction = attr->value();
        action.mArg = child->value();
        mActions.push_back(action);

        child = child->next_sibling("action");
    }

    // Now, let's get either the key or region
    child = node->first_node("touch");
    if (child)
    {
        attr = child->first_attribute("key");
        if (attr)
        {
            std::string key = attr->value();
    
            mKey = getKeyByName(key);
        }
        else
        {
            attr = child->first_attribute("x");
            if (!attr)  return;
            mActionX = atol(attr->value());
            attr = child->first_attribute("y");
            if (!attr)  return;
            mActionY = atol(attr->value());
            attr = child->first_attribute("w");
            if (!attr)  return;
            mActionW = atol(attr->value());
            attr = child->first_attribute("h");
            if (!attr)  return;
            mActionH = atol(attr->value());
        }
    }
}

int GUIAction::NotifyTouch(TOUCH_STATE state, int x, int y)
{
    if (state == TOUCH_RELEASE)
        doActions();

    return 0;
}

int GUIAction::NotifyKey(int key)
{
    if (!mKey || key != mKey)    return 1;

    doActions();
    return 0;
}

int GUIAction::NotifyVarChange(std::string varName, std::string value)
{
    if (varName.empty() && !isConditionValid() && !mKey && !mActionW)
        doActions();

    // This handles notifying the condition system of page start
    if (varName.empty() && isConditionValid())
        NotifyPageSet();

    if ((varName.empty() || IsConditionVariable(varName)) && isConditionValid() && isConditionTrue())
        doActions();

    return 0;
}

void GUIAction::simulate_progress_bar(void)
{
	ui_print("Simulating actions...\n");
	for (int i = 0; i < 5; i++)
	{
		usleep(500000);
		DataManager::SetValue("ui_progress", i * 20);
	}
}

int GUIAction::flash_zip(std::string filename, std::string pageName, const int simulate)
{
    int ret_val = 0;

	DataManager::SetValue("ui_progress", 0);

    if (filename.empty())
    {
        LOGE("No file specified.\n");
        return -1;
    }

    // We're going to jump to this page first, like a loading page
    gui_changePage(pageName);

    int fd = -1;
    ZipArchive zip;

    if (mzOpenZipArchive(filename.c_str(), &zip))
    {
        LOGE("Unable to open zip file.\n");
        return -1;
    }

    // Check the zip to see if it has a custom installer theme
	const ZipEntry* twrp = mzFindZipEntry(&zip, "META-INF/teamwin/twrp.zip");
    if (twrp != NULL)
    {
        unlink("/tmp/twrp.zip");
        fd = creat("/tmp/twrp.zip", 0666);
    }
    if (fd >= 0 && twrp != NULL && 
        mzExtractZipEntryToFile(&zip, twrp, fd) && 
        !PageManager::LoadPackage("install", "/tmp/twrp.zip"))
    {
        mzCloseZipArchive(&zip);
        PageManager::SelectPackage("install");
        gui_changePage("main");
    }
    else
    {
        // In this case, we just use the default page
        mzCloseZipArchive(&zip);
        gui_changePage(pageName);
    }
    if (fd >= 0)
        close(fd);

	if (simulate) {
		simulate_progress_bar();
	} else {
		ret_val = install_zip_package(filename.c_str());

		// Now, check if we need to ensure TWRP remains installed...
		struct stat st;
		if (stat("/sbin/installTwrp", &st) == 0)
		{
			DataManager::SetValue("tw_operation", "Configuring TWRP");
			DataManager::SetValue("tw_partition", "");
			ui_print("Configuring TWRP...\n");
			if (__system("/sbin/installTwrp reinstall") < 0)
			{
				ui_print("Unable to configure TWRP with this kernel.\n");
			}
		}
	}

    // Done
    DataManager::SetValue("ui_progress", 100);
    DataManager::SetValue("ui_progress", 0);
    return ret_val;
}

int GUIAction::doActions()
{
    if (mActions.size() < 1)    return -1;
    if (mActions.size() == 1)   return doAction(mActions.at(0), 0);
    
    // For multi-action, we always use a thread
    pthread_t t;
    pthread_create(&t, NULL, thread_start, this);

    return 0;
}

void* GUIAction::thread_start(void *cookie)
{
    GUIAction* ourThis = (GUIAction*) cookie;

	DataManager::SetValue(TW_ACTION_BUSY, 1);

    if (ourThis->mActions.size() > 1)
    {
        std::vector<Action>::iterator iter;
        for (iter = ourThis->mActions.begin(); iter != ourThis->mActions.end(); iter++)
            ourThis->doAction(*iter, 1);
    }
    else
    {
        ourThis->doAction(ourThis->mActions.at(0), 1);
    }

	DataManager::SetValue(TW_ACTION_BUSY, 0);
    return NULL;
}

void GUIAction::operation_start(const string operation_name)
{
	DataManager::SetValue(TW_ACTION_BUSY, 1);
	DataManager::SetValue("ui_progress", 0);
	DataManager::SetValue("tw_operation", operation_name);
	DataManager::SetValue("tw_operation_status", 0);
	DataManager::SetValue("tw_operation_state", 0);
}

void GUIAction::operation_end(const int operation_status, const int simulate)
{
	int simulate_fail;

	DataManager::SetValue("ui_progress", 100);
	if (simulate) {
		DataManager::GetValue(TW_SIMULATE_FAIL, simulate_fail);
		if (simulate_fail != 0)
			DataManager::SetValue("tw_operation_status", 1);
		else
			DataManager::SetValue("tw_operation_status", 0);
	} else {
		if (operation_status != 0)
			DataManager::SetValue("tw_operation_status", 1);
		else
			DataManager::SetValue("tw_operation_status", 0);
	}
	DataManager::SetValue("tw_operation_state", 1);
	DataManager::SetValue(TW_ACTION_BUSY, 0);
}

int GUIAction::doAction(Action action, int isThreaded /* = 0 */)
{
	static string zip_queue[10];
	static int zip_queue_index;
	int simulate;
	std::string arg = gui_parse_text(action.mArg);
	std::string function = gui_parse_text(action.mFunction);

	DataManager::GetValue(TW_SIMULATE_ACTIONS, simulate);

    if (function == "reboot")
    {
        curtainClose();

        sync();
        finish_recovery("s");

        if (arg == "recovery")
            tw_reboot(rb_recovery);
        else if (arg == "poweroff")
            tw_reboot(rb_poweroff);
        else if (arg == "bootloader")
            tw_reboot(rb_bootloader);
        else
            tw_reboot(rb_system);

        // This should never occur
        return -1;
    }
    if (function == "home")
    {
        PageManager::SelectPackage("TWRP");
        gui_changePage("main");
        return 0;
    }

    if (function == "key")
    {
        PageManager::NotifyKey(getKeyByName(arg));
        return 0;
    }

    if (function == "page") {
		std::string page_name = gui_parse_text(arg);
        return gui_changePage(page_name);
	}

    if (function == "reload") {
		int check = 0, ret_val = 0;
		std::string theme_path;

		operation_start("Reload Theme");
		theme_path = DataManager::GetSettingsStoragePath();
		if (ensure_path_mounted(theme_path.c_str()) < 0) {
			LOGE("Unable to mount %s during reload function startup.\n", theme_path.c_str());
			check = 1;
		}

		theme_path += "/TWRP/theme/ui.zip";
		if (check != 0 || PageManager::ReloadPackage("TWRP", theme_path) != 0)
		{
			// Loading the custom theme failed - try loading the stock theme
			LOGI("Attempting to reload stock theme...\n");
			if (PageManager::ReloadPackage("TWRP", "/res/ui.xml"))
			{
				LOGE("Failed to load base packages.\n");
				ret_val = 1;
			}
		}
        operation_end(ret_val, simulate);
	}

    if (function == "readBackup")
    {
		set_restore_files();
        return 0;
    }

    if (function == "set")
    {
        if (arg.find('=') != string::npos)
        {
            string varName = arg.substr(0, arg.find('='));
            string value = arg.substr(arg.find('=') + 1, string::npos);

            DataManager::GetValue(value, value);
            DataManager::SetValue(varName, value);
        }
        else
            DataManager::SetValue(arg, "1");
        return 0;
    }
    if (function == "clear")
    {
        DataManager::SetValue(arg, "0");
        return 0;
    }

    if (function == "mount")
    {
        if (arg == "usb")
        {
            DataManager::SetValue(TW_ACTION_BUSY, 1);
			if (!simulate)
				usb_storage_enable();
			else
				ui_print("Simulating actions...\n");
        }
        else if (!simulate)
        {
            string cmd;
			if (arg == "EXTERNAL")
				cmd = "mount " + DataManager::GetStrValue(TW_EXTERNAL_MOUNT);
			else if (arg == "INTERNAL")
				cmd = "mount " + DataManager::GetStrValue(TW_INTERNAL_MOUNT);
			else
				cmd = "mount " + arg;
            __system(cmd.c_str());
        } else
			ui_print("Simulating actions...\n");
        return 0;
    }

    if (function == "umount" || function == "unmount")
    {
        if (arg == "usb")
        {
            if (!simulate)
				usb_storage_disable();
			else
				ui_print("Simulating actions...\n");
			DataManager::SetValue(TW_ACTION_BUSY, 0);
        }
        else if (!simulate)
        {
            string cmd;
			if (arg == "EXTERNAL")
				cmd = "umount " + DataManager::GetStrValue(TW_EXTERNAL_MOUNT);
			else if (arg == "INTERNAL")
				cmd = "umount " + DataManager::GetStrValue(TW_INTERNAL_MOUNT);
			else if (DataManager::GetIntValue(TW_DONT_UNMOUNT_SYSTEM) == 1 && (arg == "system" || arg == "/system"))
				return 0;
			else
				cmd = "umount " + arg;
            __system(cmd.c_str());
        } else
			ui_print("Simulating actions...\n");
        return 0;
    }
	
	if (function == "restoredefaultsettings")
	{
		operation_start("Restore Defaults");
		if (simulate) // Simulated so that people don't accidently wipe out the "simulation is on" setting
			ui_print("Simulating actions...\n");
		else
			DataManager::ResetDefaults();
		operation_end(0, simulate);
	}
	
	if (function == "copylog")
	{
		operation_start("Copy Log");
		if (!simulate)
		{
			ensure_path_mounted("/sdcard");
			__system("cp /tmp/recovery.log /sdcard");
			sync();
			ui_print("Copied recovery log to /sdcard.\n");
		} else
			simulate_progress_bar();
		operation_end(0, simulate);
		return 0;
	}
	
	if (function == "compute" || function == "addsubtract")
	{
		if (arg.find("+") != string::npos)
        {
            string varName = arg.substr(0, arg.find('+'));
            string string_to_add = arg.substr(arg.find('+') + 1, string::npos);
			int amount_to_add = atoi(string_to_add.c_str());
			int value;

			DataManager::GetValue(varName, value);
            DataManager::SetValue(varName, value + amount_to_add);
			return 0;
        }
		if (arg.find("-") != string::npos)
        {
            string varName = arg.substr(0, arg.find('-'));
            string string_to_subtract = arg.substr(arg.find('-') + 1, string::npos);
			int amount_to_subtract = atoi(string_to_subtract.c_str());
			int value;

			DataManager::GetValue(varName, value);
			value -= amount_to_subtract;
			if (value <= 0)
				value = 0;
            DataManager::SetValue(varName, value);
			return 0;
        }
	}
	
	if (function == "setguitimezone")
	{
		string SelectedZone;
		DataManager::GetValue(TW_TIME_ZONE_GUISEL, SelectedZone); // read the selected time zone into SelectedZone
		string Zone = SelectedZone.substr(0, SelectedZone.find(';')); // parse to get time zone
		string DSTZone = SelectedZone.substr(SelectedZone.find(';') + 1, string::npos); // parse to get DST component
		
		int dst;
		DataManager::GetValue(TW_TIME_ZONE_GUIDST, dst); // check wether user chose to use DST
		
		string offset;
		DataManager::GetValue(TW_TIME_ZONE_GUIOFFSET, offset); // pull in offset
		
		string NewTimeZone = Zone;
		if (offset != "0")
			NewTimeZone += ":" + offset;
		
		if (dst != 0)
			NewTimeZone += DSTZone;
		
		DataManager::SetValue(TW_TIME_ZONE_VAR, NewTimeZone);
		update_tz_environment_variables();
		return 0;
	}

	if (function == "togglestorage") {
		if (arg == "internal") {
			DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 0);
			DataManager::SetValue(TW_STORAGE_FREE_SIZE, (int)((sdcext.sze - sdcext.used) / 1048576LLU));
		} else if (arg == "external") {
			DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 1);
			DataManager::SetValue(TW_STORAGE_FREE_SIZE, (int)((sdcint.sze - sdcint.used) / 1048576LLU));
		}
		if (ensure_path_mounted("/sdcard") == 0) {
			if (arg == "internal") {
				// Save the current zip location to the external variable
				DataManager::SetValue(TW_ZIP_EXTERNAL_VAR, DataManager::GetStrValue(TW_ZIP_LOCATION_VAR));
				// Change the current zip location to the internal variable
				DataManager::SetValue(TW_ZIP_LOCATION_VAR, DataManager::GetStrValue(TW_ZIP_INTERNAL_VAR));
			} else if (arg == "external") {
				// Save the current zip location to the internal variable
				DataManager::SetValue(TW_ZIP_INTERNAL_VAR, DataManager::GetStrValue(TW_ZIP_LOCATION_VAR));
				// Change the current zip location to the external variable
				DataManager::SetValue(TW_ZIP_LOCATION_VAR, DataManager::GetStrValue(TW_ZIP_EXTERNAL_VAR));
			}
		} else {
			// We weren't able to toggle for some reason, restore original setting
			if (arg == "internal") {
				DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 1);
			} else if (arg == "external") {
				DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 0);
			}
		}
		return 0;
	}
	
	if (function == "overlay")
        return gui_changeOverlay(arg);

	if (function == "queuezip")
    {
        if (zip_queue_index >= 10) {
			ui_print("Maximum zip queue reached!\n");
			return 0;
		}
		DataManager::GetValue("tw_filename", zip_queue[zip_queue_index]);
		if (strlen(zip_queue[zip_queue_index].c_str()) > 0) {
			zip_queue_index++;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		}
		return 0;
	}

	if (function == "cancelzip")
    {
        if (zip_queue_index <= 0) {
			ui_print("Minimum zip queue reached!\n");
			return 0;
		} else {
			zip_queue_index--;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		}
		return 0;
	}

	if (function == "queueclear")
	{
		zip_queue_index = 0;
		DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		return 0;
	}

	if (function == "sleep")
	{
		usleep(atoi(arg.c_str()));
		return 0;
	}

    if (isThreaded)
    {
        if (function == "flash")
        {
			int i, ret_val = 0;

			for (i=0; i<zip_queue_index; i++) {
				operation_start("Flashing");
		        DataManager::SetValue("tw_filename", zip_queue[i]);
		        DataManager::SetValue(TW_ZIP_INDEX, (i + 1));

				ret_val = flash_zip(zip_queue[i], arg, simulate);
				if (ret_val != 0) {
					ui_print("Error flashing zip '%s'\n", zip_queue[i].c_str());
					i = 10; // Error flashing zip - exit queue
					ret_val = 1;
				}
			}
			zip_queue_index = 0;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
			operation_end(ret_val, simulate);
            return 0;
        }
        if (function == "wipe")
        {
            operation_start("Format");
            DataManager::SetValue("tw_partition", arg);

			if (simulate) {
				simulate_progress_bar();
			} else {
				if (arg == "data")
					wipe_data(0);
				else if (arg == "battery")
					wipe_battery_stats();
				else if (arg == "rotate")
					wipe_rotate_data();
				else if (arg == "dalvik")
					wipe_dalvik_cache();
				else
					erase_volume(arg.c_str());
				
				if (arg == "/sdcard") {
					ensure_path_mounted(SDCARD_ROOT);
					mkdir("/sdcard/TWRP", 0777);
					DataManager::Flush();
				}
			}
            operation_end(0, simulate);
            return 0;
        }
		if (function == "refreshsizes")
		{
			operation_start("Refreshing Sizes");
			if (simulate) {
				simulate_progress_bar();
			} else
				update_system_details();
			operation_end(0, simulate);
		}
        if (function == "nandroid")
        {
            operation_start("Nandroid");

			if (simulate) {
				DataManager::SetValue("tw_partition", "Simulation");
				simulate_progress_bar();
			} else {
				if (arg == "backup")
					nandroid_back_exe();
				else if (arg == "restore")
					nandroid_rest_exe();
				else {
					operation_end(1, simulate);
					return -1;
				}
			}
            operation_end(0, simulate);
			return 0;
        }
		if (function == "fixpermissions")
		{
			operation_start("Fix Permissions");
            LOGI("fix permissions started!\n");
			if (simulate) {
				simulate_progress_bar();
			} else
				fix_perms();

			LOGI("fix permissions DONE!\n");
			operation_end(0, simulate);
			return 0;
		}
        if (function == "dd")
        {
            operation_start("imaging");

			if (simulate) {
				simulate_progress_bar();
			} else {
				char cmd[512];
				sprintf(cmd, "dd %s", arg.c_str());
				__system(cmd);
			}
            operation_end(0, simulate);
            return 0;
        }
		if (function == "partitionsd")
		{
			operation_start("Partition SD Card");

			if (simulate) {
				simulate_progress_bar();
			} else {
				int allow_partition;
				DataManager::GetValue(TW_ALLOW_PARTITION_SDCARD, allow_partition);
				if (allow_partition == 0) {
					ui_print("This device does not have a real SD Card!\nAborting!\n");
				} else {
					// Below seen in Koush's recovery
					char sddevice[256];
					Volume *vol = volume_for_path("/sdcard");
					strcpy(sddevice, vol->device);
					// Just need block not whole partition
					sddevice[strlen("/dev/block/mmcblkX")] = NULL;

					char es[64];
					std::string ext_format;
					int ext, swap;
					DataManager::GetValue("tw_sdext_size", ext);
					DataManager::GetValue("tw_swap_size", swap);
					DataManager::GetValue("tw_sdpart_file_system", ext_format);
					sprintf(es, "/sbin/sdparted -es %dM -ss %dM -efs %s -s > /cache/part.log",ext,swap,ext_format.c_str());
					LOGI("\nrunning script: %s\n", es);
					run_script("\nContinue partitioning?",
						   "\nPartitioning sdcard : ",
						   es,
						   "\nunable to execute parted!\n(%s)\n",
						   "\nOops... something went wrong!\nPlease check the recovery log!\n",
						   "\nPartitioning complete!\n\n",
						   "\nPartitioning aborted!\n\n", 0);
					
					// recreate TWRP folder and rewrite settings - these will be gone after sdcard is partitioned
					ensure_path_mounted(SDCARD_ROOT);
					mkdir("/sdcard/TWRP", 0777);
					DataManager::Flush();
					DataManager::SetValue(TW_ZIP_EXTERNAL_VAR, "/sdcard");
					if (DataManager::GetIntValue(TW_USE_EXTERNAL_STORAGE) == 1)
						DataManager::SetValue(TW_ZIP_LOCATION_VAR, "/sdcard");

					update_system_details();
				}
			}
			operation_end(0, simulate);
			return 0;
		}
		if (function == "installhtcdumlock")
		{
			operation_start("Install HTC Dumlock");
			if (simulate) {
				simulate_progress_bar();
			} else
				install_htc_dumlock();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "htcdumlockrestoreboot")
		{
			operation_start("HTC Dumlock Restore Boot");
			if (simulate) {
				simulate_progress_bar();
			} else
				htc_dumlock_restore_original_boot();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "htcdumlockreflashrecovery")
		{
			operation_start("HTC Dumlock Reflash Recovery");
			if (simulate) {
				simulate_progress_bar();
			} else
				htc_dumlock_reflash_recovery_to_boot();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "cmd")
		{
			int op_status = 0;

			operation_start("Command");
			ui_print("Running command: '%s'\n", arg.c_str());
			if (simulate) {
				simulate_progress_bar();
			} else {
				op_status = __system(arg.c_str());
				if (op_status != 0)
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
    }
    else
    {
        pthread_t t;
        pthread_create(&t, NULL, thread_start, this);
        return 0;
    }
    return -1;
}

int GUIAction::getKeyByName(std::string key)
{
    if (key == "home")          return KEY_HOME;
    else if (key == "menu")     return KEY_MENU;
    else if (key == "back")     return KEY_BACK;
    else if (key == "search")   return KEY_SEARCH;
    else if (key == "voldown")  return KEY_VOLUMEDOWN;
    else if (key == "volup")    return KEY_VOLUMEUP;
    else if (key == "power") {
		int ret_val;
		DataManager::GetValue(TW_POWER_BUTTON, ret_val);
		if (!ret_val)
			return KEY_POWER;
		else
			return ret_val;
	}

    return atol(key.c_str());
}

