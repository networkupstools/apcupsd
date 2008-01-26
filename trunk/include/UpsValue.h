#include "amap.h"
#include "astring.h"
#include "amutex.h"
#include "aqueue.h"
#include "defines.h"

class UpsValue
{
public:

   UpsValue()                   : _string(NULL) { assign(0L);        }
   UpsValue(const astring &val) : _string(NULL) { assign(val.str()); }
   UpsValue(const char *val)    : _string(NULL) { assign(val);       }
   UpsValue(signed long val)    : _string(NULL) { assign(val);       }

   ~UpsValue() { clear(); }

   void operator=(const astring &val)  { assign(val.str()); }
   void operator=(signed long val)     { assign(val);       }

   void operator=(const UpsValue &rhs)
   {
      if (&rhs == this) return;
      switch (rhs._type)
      {
         case TYPE_SIGNED:   assign(rhs._signed);   break;
         case TYPE_STRING:   assign(*rhs._string);  break;
      }
   }

   bool operator!=(const UpsValue &rhs) { return !(*this == rhs); }
   bool operator==(const UpsValue &rhs)
   {
      if (_type != rhs._type) return false;
      switch (_type)
      {
         case TYPE_SIGNED:   return _signed == rhs._signed;
         case TYPE_STRING:   return *_string == *rhs._string;
         default:            return false;
      }
   }

   const astring format() const
   {
      if (_type == TYPE_STRING)
      {
        return *_string;
      }
      else
      {
         astring tmp;
         tmp.format("%d", _signed);
         return tmp;
      }
   }

   const char *strval() const
      { if (_type != TYPE_STRING) abort(); else return *_string; }
   signed long lval() const
      { if (_type != TYPE_SIGNED) abort(); else return _signed; }

   operator const char*() const { return strval(); }
   operator signed long() const { return lval(); }

private:

   enum type
   {
      TYPE_SIGNED,
      TYPE_STRING
   };

   void clear() { delete _string; _string = NULL; }

   void assign(const char *val)
      { clear(); _type = TYPE_STRING; _string = new astring(val); }
   void assign(signed long val)
      { clear(); _type = TYPE_SIGNED; _signed = val; }

   type _type;
   astring *_string;
   signed long _signed;
};

struct UpsDatum
{
   int ci;
   UpsValue value;
   void operator=(const UpsDatum &rhs)
      { if (&rhs != this) { ci = rhs.ci; value = rhs.value; } }
   UpsDatum(const UpsDatum &rhs)
      { ci = rhs.ci; value = rhs.value; }
   UpsDatum(int cii, const UpsValue &val)
      { ci = cii; value = val; }
   UpsDatum() {}
};


class UpsInfo
{
public:

   UpsInfo() : _mutex("UpsInfo")
   {
      // By default assume battery is connected
      _values[CI_BatteryPresent] = true;
   }

   ~UpsInfo() {}

   void update(int ci, const UpsValue &val)
   {
      _mutex.lock();
      if (!_values.contains(ci) || _values[ci] != val)
      {
         _values[ci] = val;
         notify(ci, val);
      }
      _mutex.unlock();
   }

   bool avail(int ci) const
   {
      _mutex.lock();
      bool result = _values.contains(ci);
      _mutex.unlock();
      return result;
   }

   bool get(int ci, UpsValue &val)
   {
      _mutex.lock();
      bool result = _values.contains(ci);
      if (result)
         val = _values[ci];
      _mutex.unlock();
      return result;
   }

   UpsValue &get(int ci)
   {
      _mutex.lock();
      UpsValue &val = _values[ci];
      _mutex.unlock();
      return val;
   }

   bool getbool(int ci) const
   {
      _mutex.lock();
      amap<int, UpsValue>::const_iterator iter = _values.find(ci);
      bool result = iter != _values.end() && (*iter).lval();
      _mutex.unlock();
      return result;
   }

   bool pend(UpsDatum &val) { return _notifs.dequeue(val); }

private:

   void notify(int ci, const UpsValue &val)
      { UpsDatum datum(ci, val); _notifs.enqueue(datum); }

   mutable amutex _mutex;
   amap<int, UpsValue> _values;
   aqueue<UpsDatum> _notifs;
};
