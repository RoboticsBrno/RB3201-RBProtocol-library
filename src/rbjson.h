#pragma once

#include <unordered_map>
#include <vector>
#include <sstream>

#include "jsmn.h"

struct rb_string_view {
    char *ptr;
    int len;

    bool equal(const char *other) {
        return strlen(other) == len && memcmp(other, ptr, len) == 0;
    }
};

rb_string_view rbjson_get_string(char *buf, jsmntok_t *obj, const char *name);
int rbjson_get_int(char *buf, jsmntok_t *obj, const char *name, bool *found=NULL);


namespace rbjson {

class Object;

Object *parse(char *buf, size_t size);

class Value {
public:
    enum type_t {
        OBJECT,
        ARRAY,
        STRING,
        NUMBER,
        BOOL,
        NIL
    };

    Value(type_t type = NIL);
    virtual ~Value();

    virtual void serialize(std::stringstream& ss) const = 0;
    std::string str() const;

    type_t getType() const {
        return m_type;
    }

    bool isNil() const {
        return m_type == NIL;
    }

protected:
    type_t m_type;
};

class Array;

class Object : public Value {
public:
    static Object *parse(char *buf, size_t size);

    Object();
    ~Object();

    void serialize(std::stringstream& ss) const;

    bool contains(const char *key) const;

    Value *get(const char *key) const;
    Object *getObject(const char *key) const;
    Array *getArray(const char *key) const;
    std::string getString(const char *key, std::string def = "") const;
    int64_t getInt(const char *key, int64_t def = 0) const;
    double getDouble(const char *key, double def = 0.0) const;
    bool getBool(const char *key, bool def = false) const;

    void set(const char *key, Value *value);
    void set(const char *key, const char *string);
    void set(const char *key, const std::string& str);
    void set(const char *key, int64_t number);

    void remove(const char *key);

private:
    std::unordered_map<std::string, Value*> m_members;
};

class Array : public Value {
public:
    Array();
    ~Array();

    void serialize(std::stringstream& ss) const;

    size_t size() const { return m_items.size(); };

    Value *get(size_t idx) const;
    Object *getObject(size_t idx) const;
    Array *getArray(size_t idx) const;
    std::string getString(size_t idx, std::string def = "") const;
    int64_t getInt(size_t idx, int64_t def = 0) const;
    double getDouble(size_t idx, double def = 0.0) const;
    bool getBool(size_t idx, bool def = false) const;

    void set(size_t idx, Value *value);
    void insert(size_t idx, Value *value);
    void push_back(Value *value) {
        insert(m_items.size(), value);
    }
    void remove(size_t idx);

private:
    std::vector<Value*> m_items;
};

class String : public Value {
public:
    explicit String(const char *value = "");
    explicit String(const std::string& value);
    ~String();

    void serialize(std::stringstream& ss) const;

    const std::string& get() const { return m_value; };

private:
    std::string m_value;
};

class Number : public Value {
public:
    explicit Number(double value = 0.0);
    ~Number();

    void serialize(std::stringstream& ss) const;

    double get() const { return m_value; };

private:
    double m_value;
};

class Bool : public Value {
public:
    explicit Bool(bool value = false);
    ~Bool();

    void serialize(std::stringstream& ss) const;

    bool get() const { return m_value; };

private:
    bool m_value;
};

class Nil : public Value {
public:
    void serialize(std::stringstream& ss) const;
};

};
