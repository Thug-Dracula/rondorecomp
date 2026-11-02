; record $23 boot program, 20 sectors at $4000-$DFFF
4000  82       clx
4001  208445   jsr $4584
4004  E8       inx
4005  E058     cpx #$58
4007  D0F8     bne $4001
4009  9C003E   stz $3E00
400C  9C083E   stz $3E08
400F  9C103E   stz $3E10
4012  9C183E   stz $3E18
4015  9C203E   stz $3E20
4018  9C283E   stz $3E28
401B  9C013E   stz $3E01
401E  A902     lda #$02
4020  2069E0   jsr $E069
4023  62       cla
4024  A220     ldx #$20
4026  A01E     ldy #$1E
4028  206FE0   jsr $E06F
402B  64F5     stz $F5
402D  2099E0   jsr $E099
4030  207BE0   jsr $E07B
4033  62       cla
4034  2072E0   jsr $E072
4037  A9DF     lda #$DF
4039  8D2022   sta $2220
403C  A957     lda #$57
403E  8D2122   sta $2221
4041  A910     lda #$10
4043  8D2222   sta $2222
4046  A9DF     lda #$DF
4048  8D2322   sta $2223
404B  A959     lda #$59
404D  8D2422   sta $2224
4050  A910     lda #$10
4052  8D2522   sta $2225
4055  A902     lda #$02
4057  8D1F22   sta $221F
405A  20E4E0   jsr $E0E4
405D  ADF5FF   lda $FFF5
4060  85DB     sta $DB
4062  5304     tam #$04
4064  1A       inc a
4065  5308     tam #$08
4067  1A       inc a
4068  5310     tam #$10
406A  1A       inc a
406B  5320     tam #$20
406D  A900     lda #$00
406F  85FC     sta $FC
4071  A900     lda #$00
4073  85FD     sta $FD
4075  A903     lda #$03
4077  85FE     sta $FE
4079  A9FE     lda #$FE
407B  85FF     sta $FF
407D  A900     lda #$00
407F  85FA     sta $FA
4081  A910     lda #$10
4083  85FB     sta $FB
4085  A900     lda #$00
4087  85F8     sta $F8
4089  A9C0     lda #$C0
408B  85F9     sta $F9
408D  2009E0   jsr $E009
4090  AA       tax
4091  D0DA     bne $406D
4093  205744   jsr $4457
4096  62       cla
4097  82       clx
4098  20AEE0   jsr $E0AE
409B  E3DF7F02000020 tia $7FDF,$0002,$2000
40A2  78       sei
40A3  0307     st0 #$07
40A5  1300     st1 #$00
40A7  2300     st2 #$00
40A9  0308     st0 #$08
40AB  1300     st1 #$00
40AD  2300     st2 #$00
40AF  58       cli
40B0  9C1422   stz $2214
40B3  A910     lda #$10
40B5  8D1522   sta $2215
40B8  20A2E0   jsr $E0A2
40BB  9C1422   stz $2214
40BE  A910     lda #$10
40C0  8D1522   sta $2215
40C3  209FE0   jsr $E09F
40C6  207BE0   jsr $E07B
40C9  20D044   jsr $44D0
40CC  2096E0   jsr $E096
40CF  20E842   jsr $42E8
40D2  9C1422   stz $2214
40D5  A910     lda #$10
40D7  8D1522   sta $2215
40DA  209FE0   jsr $E09F
40DD  207BE0   jsr $E07B
40E0  A906     lda #$06
40E2  A225     ldx #$25
40E4  A042     ldy #$42
40E6  205DE0   jsr $E05D
40E9  2063E0   jsr $E063
40EC  E632     inc $32
40EE  207BE0   jsr $E07B
40F1  AD003E   lda $3E00
40F4  8D303E   sta $3E30
40F7  AD083E   lda $3E08
40FA  8D393E   sta $3E39
40FD  AD103E   lda $3E10
4100  8D423E   sta $3E42
4103  AD183E   lda $3E18
4106  8D4B3E   sta $3E4B
4109  AD203E   lda $3E20
410C  8D543E   sta $3E54
410F  AD283E   lda $3E28
4112  8D5D3E   sta $3E5D
4115  204B42   jsr $424B
4118  AD103E   lda $3E10
411B  1A       inc a
411C  2903     and #$03
411E  8D103E   sta $3E10
4121  209246   jsr $4692
4124  200B45   jsr $450B
4127  AD2D22   lda $222D
412A  2908     and #$08
412C  D010     bne $413E
412E  20E842   jsr $42E8
4131  9C1422   stz $2214
4134  A910     lda #$10
4136  8D1522   sta $2215
4139  209FE0   jsr $E09F
413C  80AB     bra $40E9
413E  2099E0   jsr $E099
4141  207BE0   jsr $E07B
4144  9C103E   stz $3E10
4147  82       clx
4148  208445   jsr $4584
414B  E8       inx
414C  E058     cpx #$58
414E  D0F8     bne $4148
4150  62       cla
4151  2072E0   jsr $E072
4154  A9DF     lda #$DF
4156  8D2022   sta $2220
4159  A953     lda #$53
415B  8D2122   sta $2221
415E  A910     lda #$10
4160  8D2222   sta $2222
4163  A9DF     lda #$DF
4165  8D2322   sta $2223
4168  A955     lda #$55
416A  8D2422   sta $2224
416D  A910     lda #$10
416F  8D2522   sta $2225
4172  A902     lda #$02
4174  8D1F22   sta $221F
4177  20E4E0   jsr $E0E4
417A  62       cla
417B  82       clx
417C  20AEE0   jsr $E0AE
417F  E3DF5F02000020 tia $5FDF,$0002,$2000
4186  78       sei
4187  0307     st0 #$07
4189  1300     st1 #$00
418B  2300     st2 #$00
418D  0308     st0 #$08
418F  1300     st1 #$00
4191  2300     st2 #$00
4193  58       cli
4194  9C1422   stz $2214
4197  A910     lda #$10
4199  8D1522   sta $2215
419C  20A2E0   jsr $E0A2
419F  9C1422   stz $2214
41A2  A910     lda #$10
41A4  8D1522   sta $2215
41A7  209FE0   jsr $E09F
41AA  207BE0   jsr $E07B
41AD  20CA46   jsr $46CA
41B0  204648   jsr $4846
41B3  20FC4A   jsr $4AFC
41B6  20A249   jsr $49A2
41B9  A937     lda #$37
41BB  8D882A   sta $2A88
41BE  A980     lda #$80
41C0  8DE82B   sta $2BE8
41C3  A958     lda #$58
41C5  8DF02C   sta $2CF0
41C8  A902     lda #$02
41CA  8D3035   sta $3530
41CD  2096E0   jsr $E096
41D0  20E842   jsr $42E8
41D3  9C1422   stz $2214
41D6  A910     lda #$10
41D8  8D1522   sta $2215
41DB  209FE0   jsr $E09F
41DE  207BE0   jsr $E07B
41E1  2063E0   jsr $E063
41E4  E632     inc $32
41E6  207BE0   jsr $E07B
41E9  AD003E   lda $3E00
41EC  8D303E   sta $3E30
41EF  AD083E   lda $3E08
41F2  8D393E   sta $3E39
41F5  AD103E   lda $3E10
41F8  8D423E   sta $3E42
41FB  AD183E   lda $3E18
41FE  8D4B3E   sta $3E4B
4201  AD203E   lda $3E20
4204  8D543E   sta $3E54
4207  AD283E   lda $3E28
420A  8D5D3E   sta $3E5D
420D  204B42   jsr $424B
4210  209246   jsr $4692
4213  4413     bsr $4228
4215  20E842   jsr $42E8
4218  9C1422   stz $2214
421B  A910     lda #$10
421D  8D1522   sta $2215
4220  209FE0   jsr $E09F
4223  80BC     bra $41E1
4228  CED834   dec $34D8
422B  D008     bne $4235
422D  CE3035   dec $3530
4230  D003     bne $4235
4232  9C882A   stz $2A88
4235  20DF46   jsr $46DF
4238  206348   jsr $4863
423B  20114B   jsr $4B11
423E  20AD49   jsr $49AD
4241  209142   jsr $4291
4244  207B49   jsr $497B
4247  20E84B   jsr $4BE8
424A  60       rts
424B  78       sei
424C  A907     lda #$07
424E  207042   jsr $4270
4251  AD393E   lda $3E39
4254  8D0200   sta $0002
4257  AD423E   lda $3E42
425A  8D0300   sta $0003
425D  A908     lda #$08
425F  207042   jsr $4270
4262  AD543E   lda $3E54
4265  8D0200   sta $0002
4268  AD5D3E   lda $3E5D
426B  8D0300   sta $0003
426E  58       cli
426F  60       rts
4270  854F     sta $4F
4272  8D0000   sta $0000
4275  60       rts
4276  0A       asl a
4277  A8       tay
4278  B9DF52   lda $52DF,y
427B  8510     sta $10
427D  B9E052   lda $52E0,y
4280  8511     sta $11
4282  4C9E44   jmp $449E
4285  A928     lda #$28
4287  8DC82B   sta $2BC8
428A  60       rts
428B  A9D8     lda #$D8
428D  8DC82B   sta $2BC8
4290  60       rts
4291  ADC82B   lda $2BC8
4294  C928     cmp #$28
4296  90ED     bcc $4285
4298  C9D8     cmp #$D8
429A  B0EF     bcs $428B
429C  AC013E   ldy $3E01
429F  D0EF     bne $4290
42A1  C980     cmp #$80
42A3  F0EB     beq $4290
42A5  9025     bcc $42CC
42A7  EE083E   inc $3E08
42AA  AD083E   lda $3E08
42AD  C901     cmp #$01
42AF  F008     beq $42B9
42B1  A8       tay
42B2  D02E     bne $42E2
42B4  EE103E   inc $3E10
42B7  8029     bra $42E2
42B9  AD103E   lda $3E10
42BC  C903     cmp #$03
42BE  D022     bne $42E2
42C0  9C083E   stz $3E08
42C3  A903     lda #$03
42C5  8D103E   sta $3E10
42C8  EE013E   inc $3E01
42CB  60       rts
42CC  CE083E   dec $3E08
42CF  AD083E   lda $3E08
42D2  C9FF     cmp #$FF
42D4  D00C     bne $42E2
42D6  CE103E   dec $3E10
42D9  1007     bpl $42E2
42DB  9C083E   stz $3E08
42DE  9C103E   stz $3E10
42E1  60       rts
42E2  A980     lda #$80
42E4  8DC82B   sta $2BC8
42E7  60       rts
42E8  62       cla
42E9  A210     ldx #$10
42EB  20AEE0   jsr $E0AE
42EE  A93F     lda #$3F
42F0  8517     sta $17
42F2  A958     lda #$58
42F4  8515     sta $15
42F6  82       clx
42F7  BD682A   lda $2A68,x
42FA  F008     beq $4304
42FC  86D9     stx $D9
42FE  4421     bsr $4321
4300  301E     bmi $4320
4302  A6D9     ldx $D9
4304  E8       inx
4305  C615     dec $15
4307  D0EE     bne $42F7
4309  A617     ldx $17
430B  F013     beq $4320
430D  1300     st1 #$00
430F  2300     st2 #$00
4311  1300     st1 #$00
4313  2300     st2 #$00
4315  1300     st1 #$00
4317  2300     st2 #$00
4319  1300     st1 #$00
431B  2300     st2 #$00
431D  CA       dex
431E  10ED     bpl $430D
4320  60       rts
4321  BDD02C   lda $2CD0,x
4324  18       clc
4325  6940     adc #$40
4327  8508     sta $08
4329  BD782C   lda $2C78,x
432C  6900     adc #$00
432E  8509     sta $09
4330  BDC82B   lda $2BC8,x
4333  18       clc
4334  6920     adc #$20
4336  850A     sta $0A
4338  BD702B   lda $2B70,x
433B  6900     adc #$00
433D  850B     sta $0B
433F  BDC02A   lda $2AC0,x
4342  850D     sta $0D
4344  BDE82F   lda $2FE8,x
4347  8514     sta $14
4349  BD182B   lda $2B18,x
434C  850E     sta $0E
434E  290F     and #$0F
4350  850F     sta $0F
4352  A99D     lda #$9D
4354  8500     sta $00
4356  A94C     lda #$4C
4358  8501     sta $01
435A  BD682A   lda $2A68,x
435D  0A       asl a
435E  A8       tay
435F  9002     bcc $4363
4361  E601     inc $01
4363  B100     lda ($00),y
4365  8510     sta $10
4367  C8       iny
4368  B100     lda ($00),y
436A  8511     sta $11
436C  B210     lda ($10)
436E  8516     sta $16
4370  D003     bne $4375
4372  4C5244   jmp $4452
4375  E610     inc $10
4377  D002     bne $437B
4379  E611     inc $11
437B  82       clx
437C  C2       cly
437D  B110     lda ($10),y
437F  8500     sta $00
4381  C8       iny
4382  6406     stz $06
4384  B110     lda ($10),y
4386  1002     bpl $438A
4388  C606     dec $06
438A  18       clc
438B  6508     adc $08
438D  8507     sta $07
438F  A506     lda $06
4391  6509     adc $09
4393  8506     sta $06
4395  F015     beq $43AC
4397  1003     bpl $439C
4399  4C4044   jmp $4440
439C  C901     cmp #$01
439E  F003     beq $43A3
43A0  4C4044   jmp $4440
43A3  A507     lda $07
43A5  C930     cmp #$30
43A7  9003     bcc $43AC
43A9  4C4044   jmp $4440
43AC  C8       iny
43AD  A500     lda $00
43AF  2901     and #$01
43B1  AA       tax
43B2  6404     stz $04
43B4  A50D     lda $0D
43B6  F00E     beq $43C6
43B8  A500     lda $00
43BA  4908     eor #$08
43BC  8500     sta $00
43BE  BD5344   lda $4453,x
43C1  38       sec
43C2  F110     sbc ($10),y
43C4  8002     bra $43C8
43C6  B110     lda ($10),y
43C8  1002     bpl $43CC
43CA  C604     dec $04
43CC  18       clc
43CD  650A     adc $0A
43CF  8505     sta $05
43D1  A504     lda $04
43D3  650B     adc $0B
43D5  8504     sta $04
43D7  F013     beq $43EC
43D9  1003     bpl $43DE
43DB  4C4044   jmp $4440
43DE  A504     lda $04
43E0  C901     cmp #$01
43E2  D05C     bne $4440
43E4  A505     lda $05
43E6  C920     cmp #$20
43E8  B056     bcs $4440
43EA  8007     bra $43F3
43EC  A505     lda $05
43EE  DD5544   cmp $4455,x
43F1  904D     bcc $4440
43F3  C8       iny
43F4  B110     lda ($10),y
43F6  8503     sta $03
43F8  C8       iny
43F9  B110     lda ($10),y
43FB  2970     and #$70
43FD  4A       lsr a
43FE  4A       lsr a
43FF  4A       lsr a
4400  4A       lsr a
4401  8502     sta $02
4403  A50E     lda $0E
4405  0A       asl a
4406  B00B     bcs $4413
4408  1011     bpl $441B
440A  B110     lda ($10),y
440C  18       clc
440D  650F     adc $0F
440F  298F     and #$8F
4411  800C     bra $441F
4413  B110     lda ($10),y
4415  2980     and #$80
4417  050F     ora $0F
4419  8004     bra $441F
441B  B110     lda ($10),y
441D  298F     and #$8F
441F  8501     sta $01
4421  A514     lda $14
4423  2980     and #$80
4425  4501     eor $01
4427  8501     sta $01
4429  C8       iny
442A  A007     ldy #$07
442C  B90020   lda $2000,y
442F  8D0200   sta $0002
4432  88       dey
4433  B90020   lda $2000,y
4436  8D0300   sta $0003
4439  88       dey
443A  10F0     bpl $442C
443C  C617     dec $17
443E  3012     bmi $4452
4440  A905     lda #$05
4442  18       clc
4443  6510     adc $10
4445  8510     sta $10
4447  9002     bcc $444B
4449  E611     inc $11
444B  C616     dec $16
444D  F003     beq $4452
444F  4C7B43   jmp $437B
4452  60       rts
4457  DA       phx
4458  5A       phy
4459  A241     ldx #$41
445B  8618     stx $18
445D  A998     lda #$98
445F  8500     sta $00
4461  A944     lda #$44
4463  8501     sta $01
4465  A940     lda #$40
4467  85FB     sta $FB
4469  B200     lda ($00)
446B  85F8     sta $F8
446D  A001     ldy #$01
446F  B100     lda ($00),y
4471  85F9     sta $F9
4473  C8       iny
4474  B100     lda ($00),y
4476  85FA     sta $FA
4478  A518     lda $18
447A  85FF     sta $FF
447C  C8       iny
447D  B100     lda ($00),y
447F  85FC     sta $FC
4481  C8       iny
4482  B100     lda ($00),y
4484  85FD     sta $FD
4486  C8       iny
4487  B100     lda ($00),y
4489  85FE     sta $FE
448B  2012E0   jsr $E012
448E  AA       tax
448F  D0D4     bne $4465
4491  62       cla
4492  202DE0   jsr $E02D
4495  7A       ply
4496  FA       plx
4497  60       rts
449E  BD6034   lda $3460,x
44A1  D026     bne $44C9
44A3  BD0834   lda $3408,x
44A6  0A       asl a
44A7  A8       tay
44A8  B110     lda ($10),y
44AA  D00A     bne $44B6
44AC  9E0834   stz $3408,x
44AF  9E6034   stz $3460,x
44B2  18       clc
44B3  A900     lda #$00
44B5  60       rts
44B6  9D6034   sta $3460,x
44B9  C8       iny
44BA  B110     lda ($10),y
44BC  9D682A   sta $2A68,x
44BF  FE0834   inc $3408,x
44C2  DE6034   dec $3460,x
44C5  38       sec
44C6  A9FF     lda #$FF
44C8  60       rts
44C9  DE6034   dec $3460,x
44CC  18       clc
44CD  A9FF     lda #$FF
44CF  60       rts
44D0  82       clx
44D1  208445   jsr $4584
44D4  BDF044   lda $44F0,x
44D7  9D682A   sta $2A68,x
44DA  BDF944   lda $44F9,x
44DD  9DC82B   sta $2BC8,x
44E0  BD0245   lda $4502,x
44E3  9DD02C   sta $2CD0,x
44E6  E8       inx
44E7  E009     cpx #$09
44E9  D0E6     bne $44D1
44EB  647A     stz $7A
44ED  647B     stz $7B
44EF  60       rts
450B  A204     ldx #$04
450D  A90A     lda #$0A
450F  207642   jsr $4276
4512  E8       inx
4513  A90B     lda #$0B
4515  207642   jsr $4276
4518  E67A     inc $7A
451A  D002     bne $451E
451C  E67B     inc $7B
451E  A57B     lda $7B
4520  C901     cmp #$01
4522  D00B     bne $452F
4524  A57A     lda $7A
4526  C9E0     cmp #$E0
4528  D005     bne $452F
452A  A92F     lda #$2F
452C  8D682A   sta $2A68
452F  A57A     lda $7A
4531  297F     and #$7F
4533  C944     cmp #$44
4535  F00B     beq $4542
4537  C940     cmp #$40
4539  F001     beq $453C
453B  60       rts
453C  A57A     lda $7A
453E  3022     bmi $4562
4540  8031     bra $4573
4542  9C6F2A   stz $2A6F
4545  9C702A   stz $2A70
4548  A9DF     lda #$DF
454A  8D2022   sta $2220
454D  A957     lda #$57
454F  8D2122   sta $2221
4552  A910     lda #$10
4554  8D2222   sta $2222
4557  9C2522   stz $2225
455A  A902     lda #$02
455C  8D1F22   sta $221F
455F  4CE4E0   jmp $E0E4
4562  A936     lda #$36
4564  8D702A   sta $2A70
4567  A9DF     lda #$DF
4569  8D2022   sta $2220
456C  A95B     lda #$5B
456E  8D2122   sta $2221
4571  80DF     bra $4552
4573  A936     lda #$36
4575  8D6F2A   sta $2A6F
4578  A9DF     lda #$DF
457A  8D2022   sta $2220
457D  A95D     lda #$5D
457F  8D2122   sta $2221
4582  80CE     bra $4552
4584  9E5027   stz $2750,x
4587  9EA827   stz $27A8,x
458A  9E102A   stz $2A10,x
458D  9E682A   stz $2A68,x
4590  9E702B   stz $2B70,x
4593  9EC82B   stz $2BC8,x
4596  9E202C   stz $2C20,x
4599  9E782C   stz $2C78,x
459C  9ED02C   stz $2CD0,x
459F  9E282D   stz $2D28,x
45A2  9EC02A   stz $2AC0,x
45A5  9E182B   stz $2B18,x
45A8  9E0028   stz $2800,x
45AB  9E802D   stz $2D80,x
45AE  9ED82D   stz $2DD8,x
45B1  9E302E   stz $2E30,x
45B4  9E882E   stz $2E88,x
45B7  9EE02E   stz $2EE0,x
45BA  9E382F   stz $2F38,x
45BD  9E902F   stz $2F90,x
45C0  9EE82F   stz $2FE8,x
45C3  9E4030   stz $3040,x
45C6  9E9830   stz $3098,x
45C9  9EF030   stz $30F0,x
45CC  9E4831   stz $3148,x
45CF  9EA031   stz $31A0,x
45D2  9EF831   stz $31F8,x
45D5  9E5032   stz $3250,x
45D8  9EA832   stz $32A8,x
45DB  9E0033   stz $3300,x
45DE  9E5833   stz $3358,x
45E1  9EB033   stz $33B0,x
45E4  9E0834   stz $3408,x
45E7  9E6034   stz $3460,x
45EA  9EB834   stz $34B8,x
45ED  9E1035   stz $3510,x
45F0  9E6835   stz $3568,x
45F3  9EC035   stz $35C0,x
45F6  9E1836   stz $3618,x
45F9  9E7036   stz $3670,x
45FC  9EC836   stz $36C8,x
45FF  9E2037   stz $3720,x
4602  9E7837   stz $3778,x
4605  9ED037   stz $37D0,x
4608  9E2838   stz $3828,x
460B  9E8038   stz $3880,x
460E  9ED838   stz $38D8,x
4611  9E3039   stz $3930,x
4614  9E8839   stz $3988,x
4617  9EE039   stz $39E0,x
461A  9E383A   stz $3A38,x
461D  9E903A   stz $3A90,x
4620  9EE83A   stz $3AE8,x
4623  60       rts
4692  A523     lda $23
4694  18       clc
4695  6522     adc $22
4697  8523     sta $23
4699  A522     lda $22
469B  18       clc
469C  6521     adc $21
469E  8522     sta $22
46A0  A523     lda $23
46A2  6521     adc $21
46A4  8523     sta $23
46A6  A521     lda $21
46A8  18       clc
46A9  6520     adc $20
46AB  8521     sta $21
46AD  A522     lda $22
46AF  6520     adc $20
46B1  8522     sta $22
46B3  A523     lda $23
46B5  6520     adc $20
46B7  8523     sta $23
46B9  E620     inc $20
46BB  D00A     bne $46C7
46BD  E621     inc $21
46BF  D006     bne $46C7
46C1  E622     inc $22
46C3  D002     bne $46C7
46C5  E623     inc $23
46C7  A523     lda $23
46C9  60       rts
46CA  A200     ldx #$00
46CC  208445   jsr $4584
46CF  A903     lda #$03
46D1  9D682A   sta $2A68,x
46D4  A9A0     lda #$A0
46D6  9DD02C   sta $2CD0,x
46D9  A940     lda #$40
46DB  9DC82B   sta $2BC8,x
46DE  60       rts
46DF  A200     ldx #$00
46E1  BDB834   lda $34B8,x
46E4  0A       asl a
46E5  A8       tay
46E6  B9F346   lda $46F3,y
46E9  8500     sta $00
46EB  B9F446   lda $46F4,y
46EE  8501     sta $01
46F0  6C0020   jmp ($2000)
4846  A201     ldx #$01
4848  208445   jsr $4584
484B  A9A0     lda #$A0
484D  9DD02C   sta $2CD0,x
4850  A901     lda #$01
4852  9D702B   sta $2B70,x
4855  9DC02A   sta $2AC0,x
4858  A940     lda #$40
485A  9DC82B   sta $2BC8,x
485D  A9FF     lda #$FF
485F  9D802D   sta $2D80,x
4862  60       rts
4863  A201     ldx #$01
4865  BDB834   lda $34B8,x
4868  0A       asl a
4869  A8       tay
486A  B97748   lda $4877,y
486D  8500     sta $00
486F  B97848   lda $4878,y
4872  8501     sta $01
4874  6C0020   jmp ($2000)
497B  A204     ldx #$04
497D  BD6835   lda $3568,x
4980  38       sec
4981  ED083E   sbc $3E08
4984  9DC82B   sta $2BC8,x
4987  BDC035   lda $35C0,x
498A  ED103E   sbc $3E10
498D  9D702B   sta $2B70,x
4990  BD7036   lda $3670,x
4993  9DD02C   sta $2CD0,x
4996  BDC836   lda $36C8,x
4999  9D782C   sta $2C78,x
499C  E8       inx
499D  E014     cpx #$14
499F  D0DC     bne $497D
49A1  60       rts
49A2  A204     ldx #$04
49A4  208445   jsr $4584
49A7  E8       inx
49A8  E014     cpx #$14
49AA  D0F8     bne $49A4
49AC  60       rts
49AD  A205     ldx #$05
49AF  BD682A   lda $2A68,x
49B2  F003     beq $49B7
49B4  20BE49   jsr $49BE
49B7  E8       inx
49B8  E8       inx
49B9  E015     cpx #$15
49BB  D0F2     bne $49AF
49BD  60       rts
49BE  BDB834   lda $34B8,x
49C1  0A       asl a
49C2  A8       tay
49C3  B9D049   lda $49D0,y
49C6  8500     sta $00
49C8  B9D149   lda $49D1,y
49CB  8501     sta $01
49CD  6C0020   jmp ($2000)
4AFC  A203     ldx #$03
4AFE  208445   jsr $4584
4B01  A901     lda #$01
4B03  9D2037   sta $3720,x
4B06  A9E0     lda #$E0
4B08  9D6835   sta $3568,x
4B0B  A902     lda #$02
4B0D  9DC035   sta $35C0,x
4B10  60       rts
4B11  A203     ldx #$03
4B13  BDB834   lda $34B8,x
4B16  0A       asl a
4B17  A8       tay
4B18  B9254B   lda $4B25,y
4B1B  8500     sta $00
4B1D  B9264B   lda $4B26,y
4B20  8501     sta $01
4B22  6C0020   jmp ($2000)
4BE8  ADC82B   lda $2BC8
4BEB  8500     sta $00
4BED  ADD02C   lda $2CD0
4BF0  8501     sta $01
4BF2  9C5032   stz $3250
4BF5  A205     ldx #$05
4BF7  BD702B   lda $2B70,x
4BFA  D031     bne $4C2D
4BFC  BDB834   lda $34B8,x
4BFF  C902     cmp #$02
4C01  B02A     bcs $4C2D
4C03  BDD02C   lda $2CD0,x
4C06  38       sec
4C07  E501     sbc $01
4C09  B003     bcs $4C0E
4C0B  49FF     eor #$FF
4C0D  1A       inc a
4C0E  C920     cmp #$20
4C10  B01B     bcs $4C2D
4C12  6408     stz $08
4C14  BDC82B   lda $2BC8,x
4C17  38       sec
4C18  E500     sbc $00
4C1A  B007     bcs $4C23
4C1C  49FF     eor #$FF
4C1E  1A       inc a
4C1F  A002     ldy #$02
4C21  8408     sty $08
4C23  C912     cmp #$12
4C25  B006     bcs $4C2D
4C27  A508     lda $08
4C29  1A       inc a
4C2A  8D5032   sta $3250
4C2D  E8       inx
4C2E  E8       inx
4C2F  E015     cpx #$15
4C31  D0C4     bne $4BF7
4C33  AD682A   lda $2A68
4C36  C90B     cmp #$0B
4C38  D00E     bne $4C48
4C3A  ADC02A   lda $2AC0
4C3D  F00A     beq $4C49
4C3F  A9D0     lda #$D0
4C41  18       clc
4C42  6500     adc $00
4C44  8500     sta $00
4C46  B00A     bcs $4C52
4C48  60       rts
4C49  A930     lda #$30
4C4B  18       clc
4C4C  6500     adc $00
4C4E  8500     sta $00
4C50  B0F6     bcs $4C48
4C52  A205     ldx #$05
4C54  9E5032   stz $3250,x
4C57  BD702B   lda $2B70,x
4C5A  D03A     bne $4C96
4C5C  BDB834   lda $34B8,x
4C5F  C902     cmp #$02
4C61  B033     bcs $4C96
4C63  BDD02C   lda $2CD0,x
4C66  38       sec
4C67  E501     sbc $01
4C69  B003     bcs $4C6E
4C6B  49FF     eor #$FF
4C6D  1A       inc a
4C6E  C908     cmp #$08
4C70  B024     bcs $4C96
4C72  BDC82B   lda $2BC8,x
4C75  38       sec
4C76  E500     sbc $00
4C78  B003     bcs $4C7D
4C7A  49FF     eor #$FF
4C7C  1A       inc a
4C7D  C924     cmp #$24
4C7F  B015     bcs $4C96
4C81  6408     stz $08
4C83  BDC82B   lda $2BC8,x
4C86  38       sec
4C87  EDC82B   sbc $2BC8
4C8A  B004     bcs $4C90
4C8C  A002     ldy #$02
4C8E  8408     sty $08
4C90  A508     lda $08
4C92  1A       inc a
4C93  9D5032   sta $3250,x
4C96  E8       inx
4C97  E8       inx
4C98  E015     cpx #$15
4C9A  D0B8     bne $4C54
4C9C  60       rts