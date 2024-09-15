#ifndef PRIORITY_CLASS_H
#define PRIORITY_CLASS_H

#define NBITS_PRIORITY_CLASS 4
/* shift priority class value left by this to get the OR-mask or shift right by this after applying the priority 
class mask PRIORITY_CLASS_MASK to get the priority class as an integer
*/
#define PRIORITY_CLASS_BITS_POS (64 - NBITS_PRIORITY_CLASS)
#define PRIORITY_CLASS_MASK (0xFFFFFFFFFFFFFFFF << PRIORITY_CLASS_BITS_POS)
#define MASK_OUT_PRIORITY_CLASS (~PRIORITY_CLASS_MASK)

#endif