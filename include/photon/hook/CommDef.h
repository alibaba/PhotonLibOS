//
// Created by jun on 24-6-23.
//

#ifndef COMMDEF_H
#define COMMDEF_H

#ifndef CLASS_FAST_PROPERTY_GETTER_DEF
#define CLASS_FAST_PROPERTY_GETTER_DEF(type, name, funName)\
private: type name;\
public: virtual inline type get##funName() const;
#endif //CLASS_FAST_PROPERTY_GETTER_DEF

#ifndef CLASS_FAST_PROPERTY_GETTER
#define CLASS_FAST_PROPERTY_GETTER(type, name, funName)\
private: type name;\
public: virtual inline type get##funName() const {return this->name;}
#endif //CLASS_FAST_PROPERTY_GETTER

#ifndef CLASS_FAST_PROPERTY_GETTER2
#define CLASS_FAST_PROPERTY_GETTER2(type, name, funName, defaultValue)\
private: type name = defaultValue;\
public: virtual inline type get##funName() const {return this->name;}
#endif //CLASS_FAST_PROPERTY_GETTER2

#ifndef CLASS_FAST_PROPERTY_COMM
#define CLASS_FAST_PROPERTY_COMM(type, name, funName)\
private: type name;\
public: virtual inline type get##funName() const {return this->name;}\
public: virtual inline void set##funName(type arg){this->name=arg;}
#endif //CLASS_FAST_PROPERTY_COMM


#ifndef CLASS_FAST_PROPERTY_COMM2
#define CLASS_FAST_PROPERTY_COMM2(type, name, funName, defaultValue)\
private: type name = defaultValue;\
public: virtual inline type get##funName() const {return this->name;}\
public: virtual inline void set##funName(type arg){this->name=arg;}
#endif //CLASS_FAST_PROPERTY_COMM2





#endif //COMMDEF_H
