140001021        int64_t var_38 = -2
140001033        int64_t lpMem = std::sys::alloc::windows::process_heap_alloc()
140001033        
14000103b        if (lpMem != 0)
14000104b            int64_t lpMem_1 = std::sys::alloc::windows::process_heap_alloc()
14000104b            
140001053            if (lpMem_1 != 0)
14000105c                int64_t rax = 0x1c
140001061                int128_t __xmm@3f8000003f8000003f8000003f800000_1 =
140001061                    __xmm@3f8000003f8000003f8000003f800000
140001061                
140001070                while (true)
140001070                    *(lpMem_1 + (rax << 2) - 0x70) =
140001070                        __xmm@3f8000003f8000003f8000003f800000_1
140001075                    *(lpMem_1 + (rax << 2) - 0x60) =
140001075                        __xmm@3f8000003f8000003f8000003f800000_1
14000107a                    *(lpMem_1 + (rax << 2) - 0x50) =
14000107a                        __xmm@3f8000003f8000003f8000003f800000_1
14000107f                    *(lpMem_1 + (rax << 2) - 0x40) =
14000107f                        __xmm@3f8000003f8000003f8000003f800000_1
140001084                    *(lpMem_1 + (rax << 2) - 0x30) =
140001084                        __xmm@3f8000003f8000003f8000003f800000_1
140001089                    *(lpMem_1 + (rax << 2) - 0x20) =
140001089                        __xmm@3f8000003f8000003f8000003f800000_1
140001089                    
140001094                    if (rax == 0x3fc)
140001094                        break
140001094                    
140001096                    *(lpMem_1 + (rax << 2) - 0x10) =
140001096                        __xmm@3f8000003f8000003f8000003f800000_1
14000109b                    *(lpMem_1 + (rax << 2)) = __xmm@3f8000003f8000003f8000003f800000_1
14000109f                    rax += 0x20
14000109f                
1400010a5                __builtin_memcpy(dest: lpMem_1 + 0xfe0, 
1400010a5                    src: "\x00\x00\x80\x3f\x00\x00\x80\x3f\x00\x00\x80\x3f\x00\x00\x80\x3f\x00"
1400010a5                "00\x80\x3f\x00\x00\x80\x3f\x00\x00\x80\x3f\x00\x00\x80\x3f", 
1400010a5                    count: 0x20)
1400010ba                int64_t lpMem_2 = std::sys::alloc::windows::process_heap_alloc()
1400010ba                
1400010c2                if (lpMem_2 != 0)
1400010cb                    int64_t rax_1 = 0x1c
1400010d0                    float __xmm@40000000400000004000000040000000_1[0x4] =
1400010d0                        __xmm@40000000400000004000000040000000
1400010d0                    
1400010e0                    while (true)
1400010e0                        *(lpMem_2 + (rax_1 << 2) - 0x70) =
1400010e0                            __xmm@40000000400000004000000040000000_1
1400010e5                        *(lpMem_2 + (rax_1 << 2) - 0x60) =
1400010e5                            __xmm@40000000400000004000000040000000_1
1400010ea                        *(lpMem_2 + (rax_1 << 2) - 0x50) =
1400010ea                            __xmm@40000000400000004000000040000000_1
1400010ef                        *(lpMem_2 + (rax_1 << 2) - 0x40) =
1400010ef                            __xmm@40000000400000004000000040000000_1
1400010f4                        *(lpMem_2 + (rax_1 << 2) - 0x30) =
1400010f4                            __xmm@40000000400000004000000040000000_1
1400010f9                        *(lpMem_2 + (rax_1 << 2) - 0x20) =
1400010f9                            __xmm@40000000400000004000000040000000_1
1400010f9                        
140001104                        if (rax_1 == 0x3fc)
140001104                            break
140001104                        
140001106                        *(lpMem_2 + (rax_1 << 2) - 0x10) =
140001106                            __xmm@40000000400000004000000040000000_1
14000110b                        *(lpMem_2 + (rax_1 << 2)) =
14000110b                            __xmm@40000000400000004000000040000000_1
14000110f                        rax_1 += 0x20
14000110f                    
140001115                    __builtin_memcpy(dest: lpMem_2 + 0xfe0, 
140001115                        src: "\x00\x00\x00\x40\x00\x00\x00\x40\x00\x00\x00\x40\x00\x00\x00\x40"
140001115                    "00\x00\x00\x40\x00\x00\x00\x40\x00\x00\x00\x40\x00\x00\x00\x40", 
140001115                        count: 0x20)
140001115                    
14000117f                    for (int64_t i = 0xc; i != 0x40c; i += 0x10)
140001135                        float zmm1[0x4] = *(lpMem_1 + (i << 2) - 0x20)
14000113f                        float temp0_1[0x4] = _mm_add_ps(*(lpMem_2 + (i << 2) - 0x30), 
14000113f                            *(lpMem_1 + (i << 2) - 0x30))
140001147                        float temp0_2[0x4] = _mm_add_ps(*(lpMem_2 + (i << 2) - 0x20), zmm1)
14000114a                        *(lpMem + (i << 2) - 0x30) = temp0_1
14000114f                        *(lpMem + (i << 2) - 0x20) = temp0_2
140001159                        zmm1 = *(lpMem_1 + (i << 2))
140001162                        float temp0_3[0x4] = _mm_add_ps(*(lpMem_2 + (i << 2) - 0x10), 
140001162                            *(lpMem_1 + (i << 2) - 0x10))
140001169                        float temp0_4[0x4] = _mm_add_ps(*(lpMem_2 + (i << 2)), zmm1)
14000116c                        *(lpMem + (i << 2) - 0x10) = temp0_3
140001171                        *(lpMem + (i << 2)) = temp0_4
140001171                    
140001181                    int64_t lpMem_3 = lpMem
14000118c                    void (* var_68)() = core::fmt::float::impl$5::fmt
140001197                    char const* const var_80 = "stdout"
14000119b                    int64_t var_78 = 6
14000119b                    
1400011ab                    if (data_140021060 != 0)
140001320                        std::sync::once_lock::On...stdio::stdout::closure_env$0>,never$>()
140001320                    
1400011c0                    TEB* gsbase
1400011c0                    void* rax_4 =
1400011c0                        *(gsbase->ThreadLocalStoragePointer + (zx.q(_tls_index) << 3))
1400011c4                    int64_t r14 = *(rax_4 + 0x18)
1400011c4                    
1400011ce                    if (r14 == 0)
1400011f6                        int64_t rax_8 = data_1400211c8
140001217                        bool z_1
140001217                        
140001217                        do
140001204                            if (rax_8 == -1)
140001319                                std::thread::id::impl$0::new::exhausted()
140001319                                noreturn
140001319                            
14000120a                            r14 = rax_8 + 1
14000120a                            
14000120e                            if (rax_8 == data_1400211c8)
14000120e                                data_1400211c8 = r14
14000120e                                z_1 = true
14000120e                            else
14000120e                                rax_8 = data_1400211c8
14000120e                                z_1 = false
140001217                        while (not(z_1))
140001219                        *(rax_4 + 0x18) = r14
140001219                        
140001226                        if (r14 == data_140021028)
140001226                            goto label_1400011dc
140001226                        
140001226                        goto label_14000122c
140001226                    
1400011da                    int32_t rax_7
1400011da                    
1400011da                    if (r14 != data_140021028)
14000122c                    label_14000122c:
14000122c                        bool z_2
14000122c                        
14000122c                        if (0 == data_140021034)
14000122c                            data_140021034 = 1
14000122c                            z_2 = true
14000122c                        else
14000122c                            int64_t rax_10
14000122c                            rax_10.b = data_140021034
14000122c                            z_2 = false
14000122c                        
140001234                        if (not(z_2))
140001338                            std::sys::sync::mutex::futex::Mutex::lock_contended()
140001338                        
14000123a                        data_140021028 = r14
140001241                        rax_7 = 1
1400011da                    else
1400011dc                    label_1400011dc:
1400011dc                        int32_t rax_6 = data_140021030
1400011dc                        
1400011e5                        if (rax_6 == 0xffffffff)
14000132a                            core::option::expect_failed()
14000132a                            noreturn
14000132a                        
1400011eb                        rax_7 = rax_6 + 1
1400011eb                    
140001246                    data_140021030 = rax_7
140001253                    int64_t* var_40 = &data_140021028
14000125b                    int64_t** var_60 = &var_40
140001283                    int64_t* rcx_4 = nullptr
140001289                    int64_t* rax_12
140001289                    
140001289                    if (core::fmt::write() == 0)
1400012a2                        if (rcx_4 != 0)
1400012a4                            core::ptr::drop_glue<std::io::error::Error>()
1400012a4                        
1400012aa                        rcx_4 = nullptr
1400012ac                        rax_12 = var_40
1400012b0                        rax_12[1].d -= 1
1400012b0                        
1400012b3                        if (rax_12[1].d != 1)
1400012b3                            goto label_1400012ca
1400012b3                        
1400012b3                        goto label_1400012b5
1400012b3                    
14000128e                    if (rcx_4 == 0)
140001355                        core::panicking::panic_fmt()
140001355                        noreturn
140001355                    
140001294                    rax_12 = var_40
140001298                    rax_12[1].d -= 1
140001298                    
14000129b                    if (rax_12[1].d != 1)
1400012ca                    label_1400012ca:
1400012ca                        
1400012cd                        if (rcx_4 == 0)
1400012ec                        label_1400012ec:
1400012ec                            HeapFree(hHeap: GetProcessHeap(), dwFlags: HEAP_NONE, 
1400012ec                                lpMem: lpMem_2)
1400012fa                            HeapFree(hHeap: GetProcessHeap(), dwFlags: HEAP_NONE, 
1400012fa                                lpMem: lpMem_1)
140001318                            return HeapFree(hHeap: GetProcessHeap(), dwFlags: HEAP_NONE, 
140001318                                lpMem)
14000129b                    else
1400012b5                    label_1400012b5:
1400012b5                        *rax_12 = 0
1400012be                        char temp0_5 = *(rax_12 + 0xc)
1400012be                        *(rax_12 + 0xc) = 0
1400012be                        
1400012c4                        if (temp0_5 != 2)
1400012c4                            goto label_1400012ca
1400012c4                        
140001367                        WakeByAddressSingle(Address: rax_12 + 0xc)
140001367                        
140001373                        if (rcx_4 == 0)
140001373                            goto label_1400012ec
140001373                    
140001379                    var_40 = rcx_4
140001381                    char const* const* var_60_1 = &var_80
14000138c                    void (* var_58_1)() = core::fmt::impl$82::fmt<str$>
140001390                    int64_t** var_50 = &var_40
14000139b                    void (* var_48)() = std::io::error::impl$7::fmt
1400013b1                    core::panicking::panic_fmt()
1400013b1                    noreturn
1400013b1        
1400013c3        alloc::raw_vec::handle_error()
1400013c3        noreturn
