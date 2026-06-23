#include <dbus/dbus.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <chrono>

#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_NAME "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define MPRIS_ROOT_INTERFACE "org.mpris.MediaPlayer2"
#define PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define DBUS_NAME "org.mpris.MediaPlayer2.kuumo"

static DBusConnection *g_conn = nullptr;
static bool g_initialized = false;
static bool g_running = false;
static std::thread g_bus_thread;
static std::mutex g_mtx;
static std::queue<int> g_buttonQueue;

static std::string g_title;
static std::string g_artist;
static std::string g_thumbnailPath;
static bool g_isList = false;
static bool g_isPlaying = false;
static uint64_t g_trackIdCounter = 1;

static void append_dict_entry_string(DBusMessageIter *dict_iter, const char *key, const char *value)
{
    DBusMessageIter entry_iter, variant_iter;
    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry_iter);

    const char *k = key;
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &k);

    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "s", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);

    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_dict_entry_string_array(DBusMessageIter *dict_iter, const char *key, const std::vector<const char*> &values)
{
    DBusMessageIter entry_iter, variant_iter, array_iter;
    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry_iter);

    const char *k = key;
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &k);

    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "as", &variant_iter);
    dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "s", &array_iter);
    for (const char *v : values)
    {
        dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_STRING, &v);
    }
    dbus_message_iter_close_container(&variant_iter, &array_iter);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);

    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_dict_entry_int64(DBusMessageIter *dict_iter, const char *key, dbus_int64_t value)
{
    DBusMessageIter entry_iter, variant_iter;
    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry_iter);

    const char *k = key;
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &k);

    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "x", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_INT64, &value);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);

    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_dict_entry_bool(DBusMessageIter *dict_iter, const char *key, bool value)
{
    DBusMessageIter entry_iter, variant_iter;
    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry_iter);

    const char *k = key;
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &k);

    dbus_int32_t b = value ? 1 : 0;
    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);

    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void append_dict_entry_double(DBusMessageIter *dict_iter, const char *key, double value)
{
    DBusMessageIter entry_iter, variant_iter;
    dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry_iter);

    const char *k = key;
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &k);

    dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "d", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_DOUBLE, &value);
    dbus_message_iter_close_container(&entry_iter, &variant_iter);

    dbus_message_iter_close_container(dict_iter, &entry_iter);
}

static void build_metadata_dict(DBusMessageIter *dict_iter)
{
    char trackId[128];
    snprintf(trackId, sizeof(trackId), "/org/mpris/MediaPlayer2/track/%lu", (unsigned long)g_trackIdCounter);

    append_dict_entry_string(dict_iter, "mpris:trackid", trackId);

    if (!g_title.empty())
        append_dict_entry_string(dict_iter, "xesam:title", g_title.c_str());

    if (!g_artist.empty())
    {
        std::vector<const char*> artists;
        artists.push_back(g_artist.c_str());
        append_dict_entry_string_array(dict_iter, "xesam:artist", artists);
    }

    if (!g_thumbnailPath.empty())
    {
        std::string artUrl;
        if (g_thumbnailPath.find("://") != std::string::npos)
            artUrl = g_thumbnailPath;
        else
            artUrl = std::string("file://") + g_thumbnailPath;
        append_dict_entry_string(dict_iter, "mpris:artUrl", artUrl.c_str());
    }

    dbus_int64_t length = 1000000;
    append_dict_entry_int64(dict_iter, "mpris:length", length);
}

static DBusHandlerResult handle_properties_get(DBusConnection *conn, DBusMessage *msg)
{
    DBusError err;
    dbus_error_init(&err);

    const char *interface_name = nullptr;
    const char *property_name = nullptr;

    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    dbus_message_iter_get_basic(&iter, &interface_name);
    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &property_name);

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_HANDLED;

    DBusMessageIter reply_iter;
    dbus_message_iter_init_append(reply, &reply_iter);

    if (strcmp(interface_name, MPRIS_PLAYER_INTERFACE) == 0)
    {
        if (strcmp(property_name, "PlaybackStatus") == 0)
        {
            const char *status = g_isPlaying ? "Playing" : "Paused";
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "s", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &status);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "Metadata") == 0)
        {
            DBusMessageIter variant_iter, dict_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "a{sv}", &variant_iter);
            dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
            build_metadata_dict(&dict_iter);
            dbus_message_iter_close_container(&variant_iter, &dict_iter);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "CanPlay") == 0)
        {
            dbus_int32_t v = 1;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "CanPause") == 0)
        {
            dbus_int32_t v = 1;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "CanGoNext") == 0)
        {
            dbus_int32_t v = g_isList ? 1 : 0;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "CanGoPrevious") == 0)
        {
            dbus_int32_t v = g_isList ? 1 : 0;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "CanControl") == 0)
        {
            dbus_int32_t v = 1;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "LoopStatus") == 0)
        {
            const char *v = "None";
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "s", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "Shuffle") == 0)
        {
            dbus_int32_t v = 0;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "Volume") == 0)
        {
            double v = 1.0;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "d", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_DOUBLE, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "Position") == 0)
        {
            dbus_int64_t v = 0;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "x", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_INT64, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "MinimumRate") == 0)
        {
            double v = 1.0;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "d", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_DOUBLE, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "MaximumRate") == 0)
        {
            double v = 1.0;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "d", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_DOUBLE, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else
        {
            dbus_message_unref(reply);
            reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "Unknown property");
            if (reply) dbus_connection_send(conn, reply, nullptr);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }
    else if (strcmp(interface_name, MPRIS_ROOT_INTERFACE) == 0)
    {
        if (strcmp(property_name, "CanQuit") == 0)
        {
            dbus_int32_t v = 0;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "CanRaise") == 0)
        {
            dbus_int32_t v = 0;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "HasTrackList") == 0)
        {
            dbus_int32_t v = 0;
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "Identity") == 0)
        {
            const char *v = "Kuumo Music Player";
            DBusMessageIter variant_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "s", &variant_iter);
            dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &v);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "SupportedUriSchemes") == 0)
        {
            DBusMessageIter variant_iter, array_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "as", &variant_iter);
            dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "s", &array_iter);
            const char *scheme = "file";
            dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_STRING, &scheme);
            dbus_message_iter_close_container(&variant_iter, &array_iter);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else if (strcmp(property_name, "SupportedMimeTypes") == 0)
        {
            DBusMessageIter variant_iter, array_iter;
            dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_VARIANT, "as", &variant_iter);
            dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "s", &array_iter);
            dbus_message_iter_close_container(&variant_iter, &array_iter);
            dbus_message_iter_close_container(&reply_iter, &variant_iter);
        }
        else
        {
            dbus_message_unref(reply);
            reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "Unknown property");
            if (reply) dbus_connection_send(conn, reply, nullptr);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }
    else
    {
        dbus_message_unref(reply);
        reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "Unknown interface");
        if (reply) dbus_connection_send(conn, reply, nullptr);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static void append_player_properties(DBusMessageIter *dict_iter)
{
    const char *playbackStatus = g_isPlaying ? "Playing" : "Paused";
    append_dict_entry_string(dict_iter, "PlaybackStatus", playbackStatus);

    const char *loopStatus = "None";
    append_dict_entry_string(dict_iter, "LoopStatus", loopStatus);

    append_dict_entry_bool(dict_iter, "Shuffle", false);
    append_dict_entry_double(dict_iter, "Volume", 1.0);
    append_dict_entry_int64(dict_iter, "Position", 0);
    append_dict_entry_bool(dict_iter, "CanPlay", true);
    append_dict_entry_bool(dict_iter, "CanPause", true);
    append_dict_entry_bool(dict_iter, "CanGoNext", g_isList);
    append_dict_entry_bool(dict_iter, "CanGoPrevious", g_isList);
    append_dict_entry_bool(dict_iter, "CanControl", true);
    append_dict_entry_double(dict_iter, "MinimumRate", 1.0);
    append_dict_entry_double(dict_iter, "MaximumRate", 1.0);

    {
        DBusMessageIter entry_iter, variant_iter, meta_dict_iter;
        dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry_iter);
        const char *key = "Metadata";
        dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "a{sv}", &variant_iter);
        dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "{sv}", &meta_dict_iter);
        build_metadata_dict(&meta_dict_iter);
        dbus_message_iter_close_container(&variant_iter, &meta_dict_iter);
        dbus_message_iter_close_container(&entry_iter, &variant_iter);
        dbus_message_iter_close_container(dict_iter, &entry_iter);
    }
}

static void append_root_properties(DBusMessageIter *dict_iter)
{
    append_dict_entry_bool(dict_iter, "CanQuit", false);
    append_dict_entry_bool(dict_iter, "CanRaise", false);
    append_dict_entry_bool(dict_iter, "HasTrackList", false);
    append_dict_entry_string(dict_iter, "Identity", "Kuumo Music Player");

    {
        DBusMessageIter entry_iter, variant_iter, array_iter;
        dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry_iter);
        const char *key = "SupportedUriSchemes";
        dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "as", &variant_iter);
        dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "s", &array_iter);
        const char *scheme = "file";
        dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_STRING, &scheme);
        dbus_message_iter_close_container(&variant_iter, &array_iter);
        dbus_message_iter_close_container(&entry_iter, &variant_iter);
        dbus_message_iter_close_container(dict_iter, &entry_iter);
    }

    {
        DBusMessageIter entry_iter, variant_iter, array_iter;
        dbus_message_iter_open_container(dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry_iter);
        const char *key = "SupportedMimeTypes";
        dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "as", &variant_iter);
        dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "s", &array_iter);
        dbus_message_iter_close_container(&variant_iter, &array_iter);
        dbus_message_iter_close_container(&entry_iter, &variant_iter);
        dbus_message_iter_close_container(dict_iter, &entry_iter);
    }
}

static DBusHandlerResult handle_properties_get_all(DBusConnection *conn, DBusMessage *msg)
{
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *interface_name = nullptr;
    dbus_message_iter_get_basic(&iter, &interface_name);

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_HANDLED;

    DBusMessageIter reply_iter, dict_iter;
    dbus_message_iter_init_append(reply, &reply_iter);
    dbus_message_iter_open_container(&reply_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);

    if (strcmp(interface_name, MPRIS_PLAYER_INTERFACE) == 0)
    {
        append_player_properties(&dict_iter);
    }
    else if (strcmp(interface_name, MPRIS_ROOT_INTERFACE) == 0)
    {
        append_root_properties(&dict_iter);
    }

    dbus_message_iter_close_container(&reply_iter, &dict_iter);

    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult message_handler(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *interface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    const char *path = dbus_message_get_path(msg);

    if (!path || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(path, MPRIS_PATH) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (interface && strcmp(interface, PROPERTIES_INTERFACE) == 0)
    {
        if (strcmp(member, "Get") == 0)
            return handle_properties_get(conn, msg);
        if (strcmp(member, "GetAll") == 0)
            return handle_properties_get_all(conn, msg);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (!interface || strcmp(interface, MPRIS_PLAYER_INTERFACE) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    int code = -1;

    if (strcmp(member, "PlayPause") == 0)
        code = g_isPlaying ? 1 : 0;
    else if (strcmp(member, "Play") == 0)
        code = 0;
    else if (strcmp(member, "Pause") == 0)
        code = 1;
    else if (strcmp(member, "Next") == 0)
        code = 2;
    else if (strcmp(member, "Previous") == 0)
        code = 3;

    if (code >= 0)
    {
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_buttonQueue.push(code);
        }

        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply)
        {
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void emit_properties_changed(const char *interface, const char *property)
{
    if (!g_conn) return;

    DBusMessage *msg = dbus_message_new_signal(
        MPRIS_PATH,
        PROPERTIES_INTERFACE,
        "PropertiesChanged");

    if (!msg) return;

    DBusMessageIter iter, dict_iter, invalidated_iter;
    dbus_message_iter_init_append(msg, &iter);

    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);

    if (strcmp(interface, MPRIS_PLAYER_INTERFACE) == 0)
    {
        if (strcmp(property, "PlaybackStatus") == 0)
        {
            const char *status = g_isPlaying ? "Playing" : "Paused";
            append_dict_entry_string(&dict_iter, "PlaybackStatus", status);
        }
        else if (strcmp(property, "Metadata") == 0)
        {
            DBusMessageIter entry_iter, variant_iter, meta_dict_iter;
            dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry_iter);
            const char *key = "Metadata";
            dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "a{sv}", &variant_iter);
            dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "{sv}", &meta_dict_iter);
            build_metadata_dict(&meta_dict_iter);
            dbus_message_iter_close_container(&variant_iter, &meta_dict_iter);
            dbus_message_iter_close_container(&entry_iter, &variant_iter);
            dbus_message_iter_close_container(&dict_iter, &entry_iter);
        }
        else if (strcmp(property, "CanGoNext") == 0 || strcmp(property, "CanGoPrevious") == 0)
        {
            append_dict_entry_bool(&dict_iter, "CanGoNext", g_isList);
            append_dict_entry_bool(&dict_iter, "CanGoPrevious", g_isList);
        }
    }

    dbus_message_iter_close_container(&iter, &dict_iter);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalidated_iter);
    dbus_message_iter_close_container(&iter, &invalidated_iter);

    dbus_connection_send(g_conn, msg, nullptr);
    dbus_message_unref(msg);
}

static void dbus_loop()
{
    while (g_running)
    {
        if (g_conn)
        {
            dbus_connection_read_write_dispatch(g_conn, 100);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

extern "C" bool smtc_init()
{
    if (g_initialized) return true;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_initialized) return true;

    DBusError err;
    dbus_error_init(&err);

    g_conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!g_conn)
    {
        fprintf(stderr, "[smtc] dbus_bus_get failed: %s\n", err.message);
        dbus_error_free(&err);
        return false;
    }

    dbus_connection_set_exit_on_disconnect(g_conn, false);

    int ret = dbus_bus_request_name(g_conn, DBUS_NAME, DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
    {
        fprintf(stderr, "[smtc] dbus_bus_request_name failed: %s (ret=%d)\n",
                err.message ? err.message : "unknown", ret);
        dbus_error_free(&err);
        dbus_connection_unref(g_conn);
        g_conn = nullptr;
        return false;
    }

    if (!dbus_connection_add_filter(g_conn, message_handler, nullptr, nullptr))
    {
        fprintf(stderr, "[smtc] dbus_connection_add_filter failed\n");
        dbus_connection_unref(g_conn);
        g_conn = nullptr;
        return false;
    }

    g_running = true;
    g_trackIdCounter = 1;
    g_isPlaying = false;
    g_isList = false;
    g_title.clear();
    g_artist.clear();
    g_thumbnailPath.clear();

    g_bus_thread = std::thread(dbus_loop);

    g_initialized = true;
    return true;
}

extern "C" int smtc_update_metadata(
    const char *title,
    const char *artist,
    const char *thumbnailPath,
    bool IsList)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_conn) return 1;

    g_title = title ? title : "";
    g_artist = artist ? artist : "";
    g_thumbnailPath = thumbnailPath ? thumbnailPath : "";
    g_isList = IsList;
    g_trackIdCounter++;

    emit_properties_changed(MPRIS_PLAYER_INTERFACE, "Metadata");
    if (g_isList)
        emit_properties_changed(MPRIS_PLAYER_INTERFACE, "CanGoNext");

    return 0;
}

extern "C" void smtc_set_playback_state(bool isPlaying)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_conn) return;

    bool changed = (g_isPlaying != isPlaying);
    g_isPlaying = isPlaying;

    if (changed)
        emit_properties_changed(MPRIS_PLAYER_INTERFACE, "PlaybackStatus");
}

extern "C" void smtc_set_enabled_buttons(
    bool play, bool pause, bool next, bool prev)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_conn) return;

    bool oldIsList = g_isList;
    g_isList = next || prev;

    if (oldIsList != g_isList)
        emit_properties_changed(MPRIS_PLAYER_INTERFACE, "CanGoNext");
}

extern "C" int smtc_poll_button()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_buttonQueue.empty()) return -1;
    int code = g_buttonQueue.front();
    g_buttonQueue.pop();
    return code;
}

extern "C" void smtc_destroy()
{
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        while (!g_buttonQueue.empty())
            g_buttonQueue.pop();

        g_initialized = false;
        g_running = false;
    }

    if (g_bus_thread.joinable())
        g_bus_thread.join();

    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (g_conn)
        {
            dbus_connection_remove_filter(g_conn, message_handler, nullptr);
            dbus_bus_release_name(g_conn, DBUS_NAME, nullptr);
            dbus_connection_unref(g_conn);
            g_conn = nullptr;
        }
    }
}
