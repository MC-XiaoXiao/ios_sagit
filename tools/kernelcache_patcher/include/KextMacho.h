#pragma once

#include <Macho.h>

class KextMacho : public Macho
{
private:
    /* data */
public:
    KextMacho(/* args */);
    using Macho::Macho;
    ~KextMacho();
};
