def reverse_bits(n, bits=64):
    """Reverses the bits of an integer n with a specified bit width."""
    return int('{:0{bits}b}'.format(n, bits=bits)[::-1], 2)

def cl_div(dividend, divisor):
    """
    Performs carry-less division (polynomial division over GF(2)).
    Returns (quotient, remainder).
    """
    if divisor == 0: return 0, 0
    dl = dividend.bit_length()
    dr = divisor.bit_length()
    if dl < dr: return 0, dividend
    
    quotient = 0
    # Loop from the highest possible shift down to 0
    for i in range(dl - dr, -1, -1):
        # If the bit at the current highest position is 1
        if (dividend >> (i + dr - 1)) & 1:
            quotient |= (1 << i)
            dividend ^= (divisor << i)
    return quotient, dividend

def cl_mul(a, b):
    """Performs carry-less multiplication (polynomial multiplication over GF(2))."""
    res = 0
    for i in range(b.bit_length()):
        if (b >> i) & 1:
            res ^= (a << i)
    return res

def extended_gcd_gf2(a, m):
    """
    Extended Euclidean Algorithm for Polynomials over GF(2).
    Finds t such that (a * t) % m == 1.
    """
    old_r, r = m, a
    old_t, t = 0, 1
    
    while r != 0:
        quotient, remainder = cl_div(old_r, r)
        old_r, r = r, remainder
        # Update t using carry-less multiplication and XOR (addition/subtraction)
        old_t, t = t, old_t ^ cl_mul(quotient, t)
    
    # If old_r (GCD) > 1, inverse doesn't exist (reducible poly sharing factors)
    # But for x^1 vs CRC poly, GCD is always 1 unless poly has factor x (it doesn't).
    return old_t

# --- Main Execution ---

# 1. Input: Reflected Polynomial (CRC64-ECMA)
# This is the standard "hex" often seen in code, missing the top x^64 bit.
poly_reflected = 0xC96C5795D7870F42

# 2. Conversion: To do math, we need Normal (MSB-first) form.
# The reflected hex represents coefficients x^63...x^0 in LSB...MSB order.
# We must reverse it and add the implicit x^64 bit at the top.
poly_normal_low_64 = reverse_bits(poly_reflected, 64)
poly_normal = (1 << 64) | poly_normal_low_64

# 3. Define x in Normal form. x^1 is simply 2 (binary 10).
x_normal = 0x2

# 4. Calculate Inverse in Normal Domain
inv_normal = extended_gcd_gf2(x_normal, poly_normal)

# 5. Conversion: Result back to Reflected form for usage.
inv_reflected = reverse_bits(inv_normal, 64)

print(f"Reflected Poly:  {hex(poly_reflected)}")
print(f"Calculated x^-1: {hex(inv_reflected)}")


