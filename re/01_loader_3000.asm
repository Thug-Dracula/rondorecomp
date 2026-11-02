; record 2 (IPL stage-2 loader), payload off 0x71800, 2KB at $3000
3000  0300     st0 #$00
3002  1300     st1 #$00
3004  2311     st2 #$11
3006  0302     st0 #$02
3008  A910     lda #$10
300A  1300     st1 #$00
300C  2300     st2 #$00
300E  3A       dec a
300F  D0F9     bne $300A
3011  0305     st0 #$05
3013  2300     st2 #$00
3015  0300     st0 #$00
3017  1300     st1 #$00
3019  2300     st2 #$00
301B  0302     st0 #$02
301D  82       clx
301E  A00F     ldy #$0F
3020  1310     st1 #$10
3022  2301     st2 #$01
3024  E8       inx
3025  D0F9     bne $3020
3027  88       dey
3028  10F6     bpl $3020
302A  A981     lda #$81
302C  85FB     sta $FB
302E  A902     lda #$02
3030  85F8     sta $F8
3032  A901     lda #$01
3034  85FC     sta $FC
3036  2006E0   jsr $E006
3039  A982     lda #$82
303B  85FB     sta $FB
303D  A922     lda #$22
303F  85F8     sta $F8
3041  A902     lda #$02
3043  85FC     sta $FC
3045  2006E0   jsr $E006
3048  205AE0   jsr $E05A
304B  E003     cpx #$03
304D  B035     bcs $3084
304F  ADF5FF   lda $FFF5
3052  85DB     sta $DB
3054  5304     tam #$04
3056  1A       inc a
3057  5308     tam #$08
3059  1A       inc a
305A  5310     tam #$10
305C  1A       inc a
305D  5320     tam #$20
305F  A900     lda #$00
3061  85FC     sta $FC
3063  A900     lda #$00
3065  85FD     sta $FD
3067  A923     lda #$23
3069  85FE     sta $FE
306B  A901     lda #$01
306D  85FF     sta $FF
306F  A900     lda #$00
3071  85FA     sta $FA
3073  A940     lda #$40
3075  85FB     sta $FB
3077  A914     lda #$14
3079  85F8     sta $F8
307B  2009E0   jsr $E009
307E  AA       tax
307F  D0DE     bne $305F
3081  4C9930   jmp $3099
3084  20DEE0   jsr $E0DE
3087  B0C6     bcs $304F
3089  85DB     sta $DB
308B  62       cla
308C  209C30   jsr $309C
308F  A5DB     lda $DB
3091  18       clc
3092  6901     adc #$01
3094  5304     tam #$04
3096  1A       inc a
3097  5308     tam #$08
3099  4C0040   jmp $4000
309C  85F8     sta $F8
309E  A905     lda #$05
30A0  85FA     sta $FA
30A2  20BDE0   jsr $E0BD
30A5  A92F     lda #$2F
30A7  18       clc
30A8  65FC     adc $FC
30AA  8500     sta $00
30AC  A931     lda #$31
30AE  65FD     adc $FD
30B0  8501     sta $01
30B2  A003     ldy #$03
30B4  B100     lda ($00),y
30B6  8508     sta $08
30B8  C8       iny
30B9  B100     lda ($00),y
30BB  8509     sta $09
30BD  B208     lda ($08)
30BF  3029     bmi $30EA
30C1  D028     bne $30EB
30C3  A001     ldy #$01
30C5  B108     lda ($08),y
30C7  18       clc
30C8  65DB     adc $DB
30CA  5310     tam #$10
30CC  C8       iny
30CD  B108     lda ($08),y
30CF  18       clc
30D0  65DB     adc $DB
30D2  5320     tam #$20
30D4  C8       iny
30D5  B108     lda ($08),y
30D7  18       clc
30D8  65DB     adc $DB
30DA  5340     tam #$40
30DC  A904     lda #$04
30DE  18       clc
30DF  6508     adc $08
30E1  8508     sta $08
30E3  62       cla
30E4  6509     adc $09
30E6  8509     sta $09
30E8  80D3     bra $30BD
30EA  60       rts
30EB  C2       cly
30EC  B100     lda ($00),y
30EE  38       sec
30EF  E932     sbc #$32
30F1  85FE     sta $FE
30F3  C8       iny
30F4  B100     lda ($00),y
30F6  E90F     sbc #$0F
30F8  85FD     sta $FD
30FA  C8       iny
30FB  B100     lda ($00),y
30FD  E900     sbc #$00
30FF  85FC     sta $FC
3101  A901     lda #$01
3103  85FF     sta $FF
3105  A900     lda #$00
3107  85FA     sta $FA
3109  A980     lda #$80
310B  85FB     sta $FB
310D  A001     ldy #$01
310F  B108     lda ($08),y
3111  18       clc
3112  65FE     adc $FE
3114  85FE     sta $FE
3116  62       cla
3117  65FD     adc $FD
3119  85FD     sta $FD
311B  62       cla
311C  65FC     adc $FC
311E  85FC     sta $FC
3120  C8       iny
3121  B108     lda ($08),y
3123  85F8     sta $F8
3125  2009E0   jsr $E009
3128  AA       tax
3129  D0C0     bne $30EB
312B  A903     lda #$03
312D  80AF     bra $30DE