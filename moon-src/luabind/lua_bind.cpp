#include "lua_bind.h"
#include "common/time.hpp"
#include "common/timer.hpp"
#include "common/hash.hpp"
#include "common/log.hpp"
#include "common/sha1.hpp"
#include "common/md5.hpp"
#include "message.hpp"
#include "server.h"
#include "worker.h"
#include "lua_buffer.hpp"
#include "services/lua_service.h"

using namespace moon;

lua_bind::lua_bind(sol::table& l)
    :lua(l)
{
}

lua_bind::~lua_bind()
{
}

const lua_bind & lua_bind::bind_timer(lua_service* s) const
{
    lua.set_function("repeated", [s](int32_t duration, int32_t times)
    {
        auto& timer = s->get_worker()->timer();
        return timer.repeat(duration, times, s->id());
    });
    lua.set_function("remove_timer", [s](timer_id_t timerid)
    {
        auto& timer = s->get_worker()->timer();
        timer.remove(timerid);
    });
    return *this;
}

static int lua_table_new(lua_State* L)
{
    auto arrn = luaL_checkinteger(L, -2);
    auto hn = luaL_checkinteger(L, -1);
    lua_createtable(L, (int)arrn, (int)hn);
    return 1;
}

static int lua_string_hash(lua_State* L)
{
    size_t l;
    auto s = luaL_checklstring(L, -1, &l);
    std::string_view sv{ s,l };
    size_t hs = moon::hash_range(sv.begin(), sv.end());
    lua_pushinteger(L, hs);
    return 1;
}

static int lua_string_hex(lua_State* L)
{
    size_t l;
    auto s = luaL_checklstring(L, -1, &l);
    std::string_view sv{ s,l };
    auto res = moon::hex_string(sv);
    lua_pushlstring(L, res.data(), res.size());
    return 1;
}

static int my_lua_print(lua_State *L) {
    lua_Debug ar;
    std::string lineinfo;
    if (lua_getstack(L, 1, &ar))
    {
        if (lua_getinfo(L, "Sl", &ar))
        {
            lineinfo.append(ar.source+1);
            lineinfo.append(":");
            lineinfo.append(std::to_string(ar.currentline));
        }
    }

    moon::log* logger = (moon::log*)lua_touserdata(L, lua_upvalueindex(1));
    auto serviceid = (uint32_t)lua_tointeger(L, lua_upvalueindex(2));
    auto lv = (moon::LogLevel)lua_tointeger(L, lua_upvalueindex(3));
    int n = lua_gettop(L);  /* number of arguments */
    int i;
    lua_getglobal(L, "tostring");
    buffer buf;
    for (i = 1; i <= n; i++) {
        const char *s;
        size_t l;
        lua_pushvalue(L, -1);  /* function to be called */
        lua_pushvalue(L, i);   /* value to print */
        lua_call(L, 1, 1);
        s = lua_tolstring(L, -1, &l);  /* get result */
        if (s == NULL)
            return luaL_error(L, "'tostring' must return a string to 'print'");
        if (i > 1) buf.write_back("\t",1);
        buf.write_back(s, l);
        lua_pop(L, 1);  /* pop result */
    }
    lua_pop(L, 1);  /* pop tostring */

    if (!lineinfo.empty())
    {
        buf.write_back("\t(", 2);
        buf.write_back(lineinfo.data(), lineinfo.size());
        buf.write_back(")", 1);
    }
    logger->logstring(true, lv, string_view_t{ buf.data(), buf.size() }, serviceid);
    return 0;
}

static void register_lua_print(sol::table& lua, moon::log* logger,uint32_t serviceid)
{
    lua_pushlightuserdata(lua.lua_state(), logger);
    lua_pushinteger(lua.lua_state(), serviceid);
    lua_pushinteger(lua.lua_state(), (int)moon::LogLevel::Info);
    lua_pushcclosure(lua.lua_state(), my_lua_print, 3);
    lua_setglobal(lua.lua_state(), "print");
    assert(lua_gettop(lua.lua_state()) == 0);
}

static void lua_extend_library(lua_State* L, lua_CFunction f, const char* gname, const char* name)
{
    lua_getglobal(L, gname);
    lua_pushcfunction(L, f);
    lua_setfield(L, -2, name);
    lua_pop(L, 1); /* pop gname table */
    assert(lua_gettop(L) == 0);
}

static void sol_extend_library(sol::table t, lua_CFunction f, const char* name, const std::function<int(lua_State* L)>& uv = nullptr)
{
    lua_State* L = t.lua_state();
    t.push();//sol table
    int upvalue = 0;
    if (uv)
    { 
        upvalue = uv(L);
    }
    lua_pushcclosure(L, f, upvalue);
    lua_setfield(L, -2, name);
    lua_pop(L, 1); //sol table
    assert(lua_gettop(L) == 0);
}

const lua_bind & lua_bind::bind_util() const
{
    lua.set_function("second", time::second);
    lua.set_function("millsecond", time::millisecond);
    lua.set_function("microsecond", time::microsecond);
    lua.set_function("time_offset", time::offset);

    lua.set_function("sha1", [](std::string_view s) {
        std::string buf(sha1::sha1_context::digest_size, '\0');
        sha1::sha1_context ctx;
        sha1::init(ctx);
        sha1::update(ctx, s.data(), s.size());
        sha1::finish(ctx, buf.data());
        return buf;
    });

    lua.set_function("md5", [](std::string_view s) {
        uint8_t buf[md5::DIGEST_BYTES] = { 0 };
        md5::md5_context ctx;
        md5::init(ctx);
        md5::update(ctx, s.data(), s.size());
        md5::finish(ctx, buf);

        std::string res(md5::DIGEST_BYTES*2,'\0');
        for (size_t i = 0; i < 16; i++) {
            int t = buf[i];
            int a = t / 16;
            int b = t % 16;
            res[i * 2] = md5::HEX[a];
            res[i * 2 + 1] = md5::HEX[b];
        }
        return res;
    });

    lua_extend_library(lua.lua_state(), lua_table_new, "table", "new");
    lua_extend_library(lua.lua_state(), lua_string_hash, "string", "hash");
    lua_extend_library(lua.lua_state(), lua_string_hex, "string", "hex");

    return *this;
}

const lua_bind & lua_bind::bind_log(moon::log* logger, uint32_t serviceid) const
{
    lua.set_function("LOGV", &moon::log::logstring, logger);
    register_lua_print(lua, logger, serviceid);
    sol_extend_library(lua, my_lua_print, "error", [logger, serviceid](lua_State* L)->int {
        lua_pushlightuserdata(L, logger);
        lua_pushinteger(L, serviceid);
        lua_pushinteger(L, (int)moon::LogLevel::Error);
        return 3;
    });
    return *this;
}

const lua_bind & lua_bind::bind_message() const
{
    //https://sol2.readthedocs.io/en/latest/functions.html?highlight=lua_CFunction
    //Note that stateless lambdas can be converted to a function pointer, 
    //so stateless lambdas similar to the form [](lua_State*) -> int { ... } will also be pushed as raw functions.
    //If you need to get the Lua state that is calling a function, use sol::this_state.

    lua.set_function("redirect", [](lua_State* L)->int
    {
        int top = lua_gettop(L);
        auto m = sol::stack::get<message*>(L, 1);
        m->set_header(sol::stack::get<std::string_view>(L, 2));
        m->set_receiver(sol::stack::get<uint32_t>(L, 3));
        m->set_type(sol::stack::get<uint8_t>(L, 4));
        if (top > 4)
        {
            m->set_sender(sol::stack::get<uint32_t>(L, 5));
            m->set_sessionid(sol::stack::get<int32_t>(L, 6));
        }
        return 0;
    });

    auto tobuffer = [](lua_State* L)->int
    {
        auto m = sol::stack::get<message*>(L, -1);
        lua_pushlightuserdata(L, (void*)m->get_buffer());
        return 1;
    };

    auto cstr = [](lua_State* L)->int
    {
        auto m = sol::stack::get<message*>(L, 1);
        int offset = 0;
        if (lua_type(L, 2) == LUA_TNUMBER)
        {
            offset = static_cast<int>(lua_tointeger(L, 2));
            if (offset > m->size())
            {
                return luaL_error(L, "out of range");
            }
        }
        lua_pushlightuserdata(L, (void*)(m->data()+ offset));
        lua_pushinteger(L, m->size()- offset);
        return 2;
    };

    lua.new_usertype<message>("message"
        , sol::call_constructor, sol::no_constructor
        , "sender", (&message::sender)
        , "sessionid", (&message::sessionid)
        , "subtype", (&message::subtype)
        , "header", (&message::header)
        , "bytes", (&message::bytes)
        , "size", (&message::size)
        , "substr", (&message::substr)
        , "buffer", tobuffer
        , "cstr", cstr
        );

    sol::table tb = lua.create();
    tb.set_function("unsafe_new", [](lua_State* L)->int {
        size_t capacity = sol::stack::get<size_t>(L, 1);
        buffer* buf = new buffer(capacity, BUFFER_HEAD_RESERVED);
        lua_pushlightuserdata(L, buf);
        return 1;
    });

    tb.set_function("delete", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) { return luaL_error(L, "null pointer"); }
        delete buf;
        return 0;
    });

    tb.set_function("clear", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) { return luaL_error(L, "null pointer"); }
        buf->clear();
        return 0;
    });

    tb.set_function("size", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) { return luaL_error(L, "null pointer"); }
        lua_pushinteger(L, buf->size());
        return 1;
    });

    tb.set_function("substr", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) { return luaL_error(L, "null pointer"); }
        auto pos = static_cast<size_t>(luaL_checkinteger(L, 2));
        auto count = static_cast<size_t>(luaL_checkinteger(L, 3));

        string_view_t sw(buf->data(), buf->size());
        string_view_t sub = sw.substr(pos, count);
        lua_pushlstring(L, sub.data(), sub.size());
        return 1;
    });

    tb.set_function("str", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) { return luaL_error(L, "null pointer"); }
        lua_pushlstring(L, buf->data(), buf->size());
        return 1;
    });

    tb.set_function("cstr", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) {return luaL_error(L, "null pointer");}
        int offset = 0;
        if (lua_type(L, 2) == LUA_TNUMBER)
        {
            offset = static_cast<int>(lua_tointeger(L, 2));
            if (offset > static_cast<int>(buf->size()))
            {
                return luaL_error(L, "out of range");
            }
        }
        lua_pushlightuserdata(L, (void*)(buf->data()+ offset));
        lua_pushinteger(L, buf->size()- offset);
        return 2;
    });

    tb.set_function("read", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) { return luaL_error(L, "null pointer"); }
        auto count = static_cast<int>(luaL_checkinteger(L, 2));
        if (count > static_cast<int>(buf->size()))
        {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "out off index");
            return 2;
        }

        lua_pushlstring(L, buf->data(), count);
        buf->seek(count);
        return 1;
    });

    tb.set_function("write_front", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) { return luaL_error(L, "null pointer"); }
        size_t len = 0;
        auto data = luaL_checklstring(L, 2, &len);
        buf->write_front(data, len);
        return 0;
    });

    tb.set_function("write_back", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) { return luaL_error(L, "null pointer"); }
        size_t len = 0;
        auto data = luaL_checklstring(L, 2, &len);
        buf->write_back(data, len);
        return 0;
    });

    tb.set_function("seek", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) { return luaL_error(L, "null pointer"); }
        auto pos = static_cast<int>(luaL_checkinteger(L, 2));
        auto origin = buffer::Current;
        if (lua_type(L, 3) == LUA_TNUMBER)
        {
            origin = static_cast<buffer::seek_origin>(luaL_checkinteger(L, 3));
        }
        buf->seek(pos, origin);
        return 0;
    });

    tb.set_function("offset_writepos", [](lua_State* L)->int {
        auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
        if (buf == NULL) { return luaL_error(L, "null pointer"); }
        auto offset = static_cast<int>(luaL_checkinteger(L, 2));
        buf->offset_writepos(offset);
        return 0;
    });

    registerlib(lua.lua_state(), "buffer", tb);
    return *this;
}

const lua_bind& lua_bind::bind_service(lua_service* s) const
{
    auto router_ = s->get_router();
    auto server_ = s->get_server();
    auto worker_ = s->get_worker();

    lua.set("null", (void*)(router_));

    lua.set_function("name", &lua_service::name, s);
    lua.set_function("id", &lua_service::id, s);
    lua.set_function("set_cb", &lua_service::set_callback, s);
    lua.set_function("cpu", &lua_service::cpu_time, s);
    lua.set_function("make_prefab", &worker::make_prefab, worker_);
    lua.set_function("send_prefab", [worker_, s](uint32_t receiver, uint32_t cacheid, const string_view_t& header, int32_t sessionid, uint8_t type) {
        worker_->send_prefab(s->id(), receiver, cacheid, header, sessionid, type);
    });
    lua.set_function("send", &router::send, router_);
    lua.set_function("new_service", &router::new_service, router_);
    lua.set_function("remove_service", &router::remove_service, router_);
    lua.set_function("runcmd", &router::runcmd, router_);
    lua.set_function("broadcast", &router::broadcast, router_);
    lua.set_function("queryservice", &router::get_unique_service, router_);
    lua.set_function("set_env", &router::set_env, router_);
    lua.set_function("get_env", &router::get_env, router_);
    lua.set_function("set_loglevel", (void(moon::log::*)(string_view_t))&log::set_level, router_->logger());
    lua.set_function("get_loglevel", &log::get_level, router_->logger());
    lua.set_function("abort", &server::stop, server_);
    lua.set_function("now", &server::now, server_);
    lua.set_function("service_count", &server::service_count, server_);
    return *this;
}

const lua_bind & lua_bind::bind_socket(lua_service* s) const
{
    auto w = s->get_worker();
    auto& sock = w->socket();

    sol::table tb = lua.create();

    tb.set_function("listen", [&sock,s](const std::string& host, uint16_t port, uint8_t type) {
        return sock.listen(host, port, s->id(), type);
    });

    tb.set_function("accept", &moon::socket::accept,&sock);
    tb.set_function("connect", &moon::socket::connect, &sock);
    tb.set_function("read", &moon::socket::read, &sock);
    tb.set_function("write", &moon::socket::write, &sock);
    tb.set_function("write_with_flag", &moon::socket::write_with_flag, &sock);
    tb.set_function("write_message", &moon::socket::write_message, &sock);
    tb.set_function("close", [&sock](uint32_t fd) {sock.close(fd);});
    tb.set_function("settimeout", &moon::socket::settimeout, &sock);
    tb.set_function("setnodelay", &moon::socket::setnodelay, &sock);
    tb.set_function("set_enable_frame", &moon::socket::set_enable_frame, &sock);
    tb.set_function("set_send_queue_limit", &moon::socket::set_send_queue_limit, &sock);
	tb.set_function("getaddress", [&sock](uint32_t fd) {
		return sock.getaddress(fd);
	});

    registerlib(lua.lua_state(), "asio", tb);
    return *this;
}

const lua_bind & lua_bind::bind_datetime(lua_service * s) const
{
    auto ser = s->get_server();
    auto& dt = ser->get_datetime();

    sol::table tb = lua.create_named("datetime");

    tb.set_function("localday", &moon::datetime::localday,&dt);
    tb.set_function("localday_off", &moon::datetime::localday_off, &dt);
    tb.set_function("year", &moon::datetime::year, &dt);
    tb.set_function("month", &moon::datetime::month, &dt);
    tb.set_function("day", &moon::datetime::day, &dt);
    tb.set_function("hour", &moon::datetime::hour, &dt);
    tb.set_function("minutes", &moon::datetime::minutes, &dt);
    tb.set_function("seconds", &moon::datetime::seconds, &dt);
    tb.set_function("weekday", &moon::datetime::weekday, &dt);
    tb.set_function("is_leap_year", &moon::datetime::is_leap_year, &dt);
    tb.set_function("is_same_day", &moon::datetime::is_same_day, &dt);
    tb.set_function("is_same_week", &moon::datetime::is_same_week, &dt);
    tb.set_function("is_same_month", &moon::datetime::is_same_month, &dt);
    tb.set_function("past_day", &moon::datetime::past_day, &dt);
    tb.set_function("timezone", &moon::datetime::timezone, &dt);

    registerlib(lua.lua_state(), "datetimecore", tb);
    return *this;
}

void lua_bind::registerlib(lua_State * L, const char * name, lua_CFunction f)
{
    luaL_requiref(L, name, f, 0);
    lua_pop(L, 1); /* pop result*/
    assert(lua_gettop(L) == 0);
}

void lua_bind::registerlib(lua_State * L, const char * name, const sol::table& module)
{
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded"); /* get 'package.loaded' */
    module.push();
    lua_setfield(L, -2, name); /* package.loaded[name] = f */
    lua_pop(L, 2); /* pop 'package' and 'loaded' tables */
    assert(lua_gettop(L) == 0);
}

