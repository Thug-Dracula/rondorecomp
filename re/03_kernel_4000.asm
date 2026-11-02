; game kernel module: disc sector 67 (payload 0x92000), 12KB at $4000-$6FFF, 81 exported entry points
401B  4C9343   jmp $4393
4021  4C615B   jmp $5B61
4027  4C595B   jmp $5B59
402D  4C6C5B   jmp $5B6C
4045  4CD255   jmp $55D2
405D  4CE759   jmp $59E7
4075  4C035B   jmp $5B03
40AE  4CCB62   jmp $62CB
40F9  F303400020DB00 tai $4003,$2000,$00DB
4100  F30340D022301D tai $4003,$22D0,$1D30
4107  62       cla
4108  A220     ldx #$20
410A  A01E     ldy #$1E
410C  206FE0   jsr $E06F
410F  62       cla
4110  206CE0   jsr $E06C
4113  9C1422   stz $2214
4116  A910     lda #$10
4118  8D1522   sta $2215
411B  20A2E0   jsr $E0A2
411E  A900     lda #$00
4120  8530     sta $30
4122  A90D     lda #$0D
4124  18       clc
4125  65DB     adc $DB
4127  5310     tam #$10
4129  1A       inc a
412A  5320     tam #$20
412C  1A       inc a
412D  5340     tam #$40
412F  2033B2   jsr $B233
4132  20D442   jsr $42D4
4135  A90D     lda #$0D
4137  20665B   jsr $5B66
413A  A9E0     lda #$E0
413C  8D0200   sta $0002
413F  A904     lda #$04
4141  20D86F   jsr $6FD8
4144  A906     lda #$06
4146  20D86F   jsr $6FD8
4149  A924     lda #$24
414B  20436E   jsr $6E43
414E  A9FF     lda #$FF
4150  85A2     sta $A2
4152  A901     lda #$01
4154  A277     ldx #$77
4156  A041     ldy #$41
4158  205DE0   jsr $E05D
415B  A902     lda #$02
415D  A297     ldx #$97
415F  A042     ldy #$42
4161  205DE0   jsr $E05D
4164  A906     lda #$06
4166  85F5     sta $F5
4168  A903     lda #$03
416A  18       clc
416B  65DB     adc $DB
416D  5310     tam #$10
416F  642F     stz $2F
4171  4C7441   jmp $4174
4174  4C7441   jmp $4174
42D4  20645B   jsr $5B64
42D7  A9C8     lda #$C8
42D9  854A     sta $4A
42DB  290F     and #$0F
42DD  8D0200   sta $0002
42E0  62       cla
42E1  854B     sta $4B
42E3  8D0300   sta $0003
42E6  9C083E   stz $3E08
42E9  9C103E   stz $3E10
42EC  9C203E   stz $3E20
42EF  9C283E   stz $3E28
42F2  6445     stz $45
42F4  A904     lda #$04
42F6  8539     sta $39
42F8  60       rts
433F  DA       phx
4340  5A       phy
4341  4310     tma #$10
4343  48       pha
4344  4320     tma #$20
4346  48       pha
4347  4340     tma #$40
4349  48       pha
434A  A90D     lda #$0D
434C  18       clc
434D  65DB     adc $DB
434F  5310     tam #$10
4351  1A       inc a
4352  5320     tam #$20
4354  1A       inc a
4355  5340     tam #$40
4357  205AB2   jsr $B25A
435A  68       pla
435B  5340     tam #$40
435D  68       pla
435E  5320     tam #$20
4360  68       pla
4361  5310     tam #$10
4363  7A       ply
4364  FA       plx
4365  60       rts
4393  2434     bit $34
4395  3036     bmi $43CD
4397  48       pha
4398  A980     lda #$80
439A  8534     sta $34
439C  4310     tma #$10
439E  8D0C3F   sta $3F0C
43A1  4320     tma #$20
43A3  8D0D3F   sta $3F0D
43A6  4340     tma #$40
43A8  8D0E3F   sta $3F0E
43AB  A90D     lda #$0D
43AD  18       clc
43AE  65DB     adc $DB
43B0  5310     tam #$10
43B2  1A       inc a
43B3  5320     tam #$20
43B5  1A       inc a
43B6  5340     tam #$40
43B8  68       pla
43B9  2084B2   jsr $B284
43BC  AD0E3F   lda $3F0E
43BF  5340     tam #$40
43C1  AD0D3F   lda $3F0D
43C4  5320     tam #$20
43C6  AD0C3F   lda $3F0C
43C9  5310     tam #$10
43CB  6434     stz $34
43CD  60       rts
43D8  643A     stz $3A
43DA  5F3AFD   bbr5 $3A,$00FD
43DD  9C313F   stz $3F31
43E0  20645B   jsr $5B64
43E3  A54A     lda $4A
43E5  293F     and #$3F
43E7  8D303F   sta $3F30
43EA  8D0200   sta $0002
43ED  60       rts
5506  F30340803C7F00 tai $4003,$3C80,$007F
550D  60       rts
5514  9D803C   sta $3C80,x
5517  9E903C   stz $3C90,x
551A  9E983C   stz $3C98,x
551D  60       rts
551E  82       clx
551F  9D883C   sta $3C88,x
5522  A902     lda #$02
5524  80EE     bra $5514
5526  9D883C   sta $3C88,x
5529  A903     lda #$03
552B  80E7     bra $5514
552D  82       clx
552E  A8       tay
552F  443D     bsr $556E
5531  A901     lda #$01
5533  9D003C   sta $3C00,x
5536  98       tya
5537  9D0C3C   sta $3C0C,x
553A  60       rts
553B  82       clx
553C  A8       tay
553D  442F     bsr $556E
553F  A902     lda #$02
5541  80F0     bra $5533
5543  82       clx
5544  A8       tay
5545  4427     bsr $556E
5547  A903     lda #$03
5549  80E8     bra $5533
554B  A8       tay
554C  4420     bsr $556E
554E  A904     lda #$04
5550  80E1     bra $5533
5552  A8       tay
5553  AD033C   lda $3C03
5556  D008     bne $5560
5558  4414     bsr $556E
555A  A905     lda #$05
555C  44D5     bsr $5533
555E  38       sec
555F  60       rts
5560  18       clc
5561  60       rts
556E  9E003C   stz $3C00,x
5571  9E0C3C   stz $3C0C,x
5574  9E183C   stz $3C18,x
5577  9E243C   stz $3C24,x
557A  9E303C   stz $3C30,x
557D  9E3C3C   stz $3C3C,x
5580  9E483C   stz $3C48,x
5583  9E543C   stz $3C54,x
5586  60       rts
5587  A58E     lda $8E
5589  2918     and #$18
558B  D005     bne $5592
558D  A90C     lda #$0C
558F  202DE0   jsr $E02D
5592  A9FF     lda #$FF
5594  4C1B40   jmp $401B
5597  F8       sed
5598  18       clc
5599  6524     adc $24
559B  8524     sta $24
559D  A525     lda $25
559F  8500     sta $00
55A1  62       cla
55A2  800E     bra $55B2
55A4  F8       sed
55A5  A500     lda $00
55A7  18       clc
55A8  6524     adc $24
55AA  8524     sta $24
55AC  A525     lda $25
55AE  8500     sta $00
55B0  A501     lda $01
55B2  6525     adc $25
55B4  8525     sta $25
55B6  62       cla
55B7  6526     adc $26
55B9  8526     sta $26
55BB  9008     bcc $55C5
55BD  A999     lda #$99
55BF  8524     sta $24
55C1  8525     sta $25
55C3  8526     sta $26
55C5  D8       cld
55C6  A500     lda $00
55C8  4525     eor $25
55CA  29A0     and #$A0
55CC  F002     beq $55D0
55CE  38       sec
55CF  60       rts
55D0  18       clc
55D1  60       rts
55D2  6410     stz $10
55D4  BD802D   lda $2D80,x
55D7  1002     bpl $55DB
55D9  C610     dec $10
55DB  BDD82D   lda $2DD8,x
55DE  18       clc
55DF  7D202C   adc $2C20,x
55E2  9D202C   sta $2C20,x
55E5  BD802D   lda $2D80,x
55E8  7DC82B   adc $2BC8,x
55EB  9DC82B   sta $2BC8,x
55EE  A510     lda $10
55F0  7D702B   adc $2B70,x
55F3  9D702B   sta $2B70,x
55F6  6410     stz $10
55F8  BD302E   lda $2E30,x
55FB  1002     bpl $55FF
55FD  C610     dec $10
55FF  BD882E   lda $2E88,x
5602  18       clc
5603  7D282D   adc $2D28,x
5606  9D282D   sta $2D28,x
5609  BD302E   lda $2E30,x
560C  7DD02C   adc $2CD0,x
560F  9DD02C   sta $2CD0,x
5612  A510     lda $10
5614  7D782C   adc $2C78,x
5617  9D782C   sta $2C78,x
561A  60       rts
561B  6410     stz $10
561D  BD802D   lda $2D80,x
5620  1002     bpl $5624
5622  C610     dec $10
5624  BDD82D   lda $2DD8,x
5627  18       clc
5628  7D202C   adc $2C20,x
562B  9D202C   sta $2C20,x
562E  BD802D   lda $2D80,x
5631  7DC82B   adc $2BC8,x
5634  9DC82B   sta $2BC8,x
5637  A510     lda $10
5639  7D702B   adc $2B70,x
563C  9D702B   sta $2B70,x
563F  60       rts
5640  48       pha
5641  205D56   jsr $565D
5644  A515     lda $15
5646  9D802D   sta $2D80,x
5649  A514     lda $14
564B  9DD82D   sta $2DD8,x
564E  68       pla
564F  206056   jsr $5660
5652  A515     lda $15
5654  9D302E   sta $2E30,x
5657  A514     lda $14
5659  9D882E   sta $2E88,x
565C  60       rts
565D  18       clc
565E  6940     adc #$40
5660  0A       asl a
5661  B00F     bcs $5672
5663  A8       tay
5664  B9CF56   lda $56CF,y
5667  8516     sta $16
5669  B9D056   lda $56D0,y
566C  8517     sta $17
566E  209A56   jsr $569A
5671  60       rts
5672  A8       tay
5673  B9CF56   lda $56CF,y
5676  8516     sta $16
5678  B9D056   lda $56D0,y
567B  8517     sta $17
567D  209A56   jsr $569A
5680  A513     lda $13
5682  49FF     eor #$FF
5684  18       clc
5685  6901     adc #$01
5687  8513     sta $13
5689  A514     lda $14
568B  49FF     eor #$FF
568D  6900     adc #$00
568F  8514     sta $14
5691  A515     lda $15
5693  49FF     eor #$FF
5695  6900     adc #$00
5697  8515     sta $15
5699  60       rts
569A  6413     stz $13
569C  6414     stz $14
569E  6415     stz $15
56A0  A500     lda $00
56A2  8510     sta $10
56A4  A501     lda $01
56A6  8511     sta $11
56A8  6412     stz $12
56AA  A009     ldy #$09
56AC  4617     lsr $17
56AE  6616     ror $16
56B0  9013     bcc $56C5
56B2  A513     lda $13
56B4  18       clc
56B5  6510     adc $10
56B7  8513     sta $13
56B9  A514     lda $14
56BB  6511     adc $11
56BD  8514     sta $14
56BF  A515     lda $15
56C1  6512     adc $12
56C3  8515     sta $15
56C5  0610     asl $10
56C7  2611     rol $11
56C9  2612     rol $12
56CB  88       dey
56CC  D0DE     bne $56AC
56CE  60       rts
57CF  A508     lda $08
57D1  38       sec
57D2  FDC82B   sbc $2BC8,x
57D5  8500     sta $00
57D7  A509     lda $09
57D9  FD702B   sbc $2B70,x
57DC  8501     sta $01
57DE  A50A     lda $0A
57E0  38       sec
57E1  FDD02C   sbc $2CD0,x
57E4  8502     sta $02
57E6  A50B     lda $0B
57E8  FD782C   sbc $2C78,x
57EB  8503     sta $03
57ED  8060     bra $584F
57EF  ADC82B   lda $2BC8
57F2  38       sec
57F3  FDC82B   sbc $2BC8,x
57F6  8500     sta $00
57F8  AD702B   lda $2B70
57FB  FD702B   sbc $2B70,x
57FE  8501     sta $01
5800  ADD02C   lda $2CD0
5803  38       sec
5804  FDD02C   sbc $2CD0,x
5807  8502     sta $02
5809  AD782C   lda $2C78
580C  FD782C   sbc $2C78,x
580F  8503     sta $03
5811  A500     lda $00
5813  18       clc
5814  6508     adc $08
5816  8500     sta $00
5818  A501     lda $01
581A  6509     adc $09
581C  8501     sta $01
581E  A502     lda $02
5820  18       clc
5821  650A     adc $0A
5823  8502     sta $02
5825  A503     lda $03
5827  650B     adc $0B
5829  8503     sta $03
582B  8022     bra $584F
582D  ADC82B   lda $2BC8
5830  38       sec
5831  FDC82B   sbc $2BC8,x
5834  8500     sta $00
5836  AD702B   lda $2B70
5839  FD702B   sbc $2B70,x
583C  8501     sta $01
583E  ADD02C   lda $2CD0
5841  38       sec
5842  FDD02C   sbc $2CD0,x
5845  8502     sta $02
5847  AD782C   lda $2C78
584A  FD782C   sbc $2C78,x
584D  8503     sta $03
584F  A503     lda $03
5851  3021     bmi $5874
5853  A501     lda $01
5855  3002     bmi $5859
5857  805F     bra $58B8
5859  A500     lda $00
585B  49FF     eor #$FF
585D  18       clc
585E  6901     adc #$01
5860  8500     sta $00
5862  A501     lda $01
5864  49FF     eor #$FF
5866  6900     adc #$00
5868  8501     sta $01
586A  444C     bsr $58B8
586C  38       sec
586D  E980     sbc #$80
586F  49FF     eor #$FF
5871  1A       inc a
5872  1A       inc a
5873  60       rts
5874  A501     lda $01
5876  1028     bpl $58A0
5878  A500     lda $00
587A  49FF     eor #$FF
587C  18       clc
587D  6901     adc #$01
587F  8500     sta $00
5881  A501     lda $01
5883  49FF     eor #$FF
5885  6900     adc #$00
5887  8501     sta $01
5889  A502     lda $02
588B  49FF     eor #$FF
588D  18       clc
588E  6901     adc #$01
5890  8502     sta $02
5892  A503     lda $03
5894  49FF     eor #$FF
5896  6900     adc #$00
5898  8503     sta $03
589A  441C     bsr $58B8
589C  18       clc
589D  6980     adc #$80
589F  60       rts
58A0  A502     lda $02
58A2  49FF     eor #$FF
58A4  18       clc
58A5  6901     adc #$01
58A7  8502     sta $02
58A9  A503     lda $03
58AB  49FF     eor #$FF
58AD  6900     adc #$00
58AF  8503     sta $03
58B1  4405     bsr $58B8
58B3  49FF     eor #$FF
58B5  1A       inc a
58B6  1A       inc a
58B7  60       rts
58B8  A501     lda $01
58BA  0503     ora $03
58BC  F00B     beq $58C9
58BE  4601     lsr $01
58C0  6600     ror $00
58C2  4603     lsr $03
58C4  6602     ror $02
58C6  4A       lsr a
58C7  D0F5     bne $58BE
58C9  A500     lda $00
58CB  0502     ora $02
58CD  89F0     bit #$F0
58CF  F009     beq $58DA
58D1  4600     lsr $00
58D3  4602     lsr $02
58D5  4A       lsr a
58D6  89F0     bit #$F0
58D8  D0F7     bne $58D1
58DA  A502     lda $02
58DC  0A       asl a
58DD  0A       asl a
58DE  0A       asl a
58DF  0A       asl a
58E0  0500     ora $00
58E2  A8       tay
58E3  B9E758   lda $58E7,y
58E6  60       rts
59E7  02       sxy
59E8  20ED59   jsr $59ED
59EB  02       sxy
59EC  60       rts
59ED  9E5027   stz $2750,x
59F0  9EA827   stz $27A8,x
59F3  9E102A   stz $2A10,x
59F6  9E682A   stz $2A68,x
59F9  9E702B   stz $2B70,x
59FC  9EC82B   stz $2BC8,x
59FF  9E202C   stz $2C20,x
5A02  9E782C   stz $2C78,x
5A05  9ED02C   stz $2CD0,x
5A08  9E282D   stz $2D28,x
5A0B  9EC02A   stz $2AC0,x
5A0E  9E182B   stz $2B18,x
5A11  9E0028   stz $2800,x
5A14  9E802D   stz $2D80,x
5A17  9ED82D   stz $2DD8,x
5A1A  9E302E   stz $2E30,x
5A1D  9E882E   stz $2E88,x
5A20  9EE02E   stz $2EE0,x
5A23  9E382F   stz $2F38,x
5A26  9E902F   stz $2F90,x
5A29  9EE82F   stz $2FE8,x
5A2C  9E4030   stz $3040,x
5A2F  9E9830   stz $3098,x
5A32  9EF030   stz $30F0,x
5A35  9E4831   stz $3148,x
5A38  9EA031   stz $31A0,x
5A3B  9EF831   stz $31F8,x
5A3E  9E5032   stz $3250,x
5A41  9EA832   stz $32A8,x
5A44  9E0033   stz $3300,x
5A47  9E5833   stz $3358,x
5A4A  9EB033   stz $33B0,x
5A4D  9E0834   stz $3408,x
5A50  9E6034   stz $3460,x
5A53  9EB834   stz $34B8,x
5A56  9E1035   stz $3510,x
5A59  9E6835   stz $3568,x
5A5C  9EC035   stz $35C0,x
5A5F  9E1836   stz $3618,x
5A62  9E7036   stz $3670,x
5A65  9EC836   stz $36C8,x
5A68  9E2037   stz $3720,x
5A6B  9E7837   stz $3778,x
5A6E  9ED037   stz $37D0,x
5A71  9E2838   stz $3828,x
5A74  9E8038   stz $3880,x
5A77  9ED838   stz $38D8,x
5A7A  9E3039   stz $3930,x
5A7D  9E8839   stz $3988,x
5A80  9EE039   stz $39E0,x
5A83  9E383A   stz $3A38,x
5A86  9E903A   stz $3A90,x
5A89  9EE83A   stz $3AE8,x
5A8C  60       rts
5A8D  DA       phx
5A8E  A210     ldx #$10
5A90  20ED59   jsr $59ED
5A93  E8       inx
5A94  E054     cpx #$54
5A96  90F8     bcc $5A90
5A98  FA       plx
5A99  60       rts
5A9A  A523     lda $23
5A9C  18       clc
5A9D  6522     adc $22
5A9F  8523     sta $23
5AA1  A522     lda $22
5AA3  18       clc
5AA4  6521     adc $21
5AA6  8522     sta $22
5AA8  A523     lda $23
5AAA  6521     adc $21
5AAC  8523     sta $23
5AAE  A521     lda $21
5AB0  18       clc
5AB1  6520     adc $20
5AB3  8521     sta $21
5AB5  A522     lda $22
5AB7  6520     adc $20
5AB9  8522     sta $22
5ABB  A523     lda $23
5ABD  6520     adc $20
5ABF  8523     sta $23
5AC1  E620     inc $20
5AC3  D00A     bne $5ACF
5AC5  E621     inc $21
5AC7  D006     bne $5ACF
5AC9  E622     inc $22
5ACB  D002     bne $5ACF
5ACD  E623     inc $23
5ACF  A523     lda $23
5AD1  60       rts
5AD2  A501     lda $01
5AD4  C502     cmp $02
5AD6  F01E     beq $5AF6
5AD8  A501     lda $01
5ADA  300D     bmi $5AE9
5ADC  C502     cmp $02
5ADE  B017     bcs $5AF7
5AE0  18       clc
5AE1  6980     adc #$80
5AE3  C502     cmp $02
5AE5  9010     bcc $5AF7
5AE7  8014     bra $5AFD
5AE9  C502     cmp $02
5AEB  9010     bcc $5AFD
5AED  38       sec
5AEE  E980     sbc #$80
5AF0  C502     cmp $02
5AF2  B009     bcs $5AFD
5AF4  8001     bra $5AF7
5AF6  60       rts
5AF7  A501     lda $01
5AF9  38       sec
5AFA  E500     sbc $00
5AFC  60       rts
5AFD  A501     lda $01
5AFF  18       clc
5B00  6500     adc $00
5B02  60       rts
5B03  DA       phx
5B04  48       pha
5B05  4A       lsr a
5B06  4A       lsr a
5B07  4A       lsr a
5B08  A8       tay
5B09  68       pla
5B0A  2907     and #$07
5B0C  AA       tax
5B0D  A901     lda #$01
5B0F  E000     cpx #$00
5B11  F004     beq $5B17
5B13  0A       asl a
5B14  CA       dex
5B15  D0FC     bne $5B13
5B17  FA       plx
5B18  60       rts
5B19  BD382F   lda $2F38,x
5B1C  2980     and #$80
5B1E  F00C     beq $5B2C
5B20  BD9830   lda $3098,x
5B23  20035B   jsr $5B03
5B26  19D326   ora $26D3,y
5B29  99D326   sta $26D3,y
5B2C  60       rts
5B2D  BDD82D   lda $2DD8,x
5B30  49FF     eor #$FF
5B32  18       clc
5B33  6901     adc #$01
5B35  9DD82D   sta $2DD8,x
5B38  BD802D   lda $2D80,x
5B3B  49FF     eor #$FF
5B3D  6900     adc #$00
5B3F  9D802D   sta $2D80,x
5B42  60       rts
5B43  BD882E   lda $2E88,x
5B46  49FF     eor #$FF
5B48  18       clc
5B49  6901     adc #$01
5B4B  9D882E   sta $2E88,x
5B4E  BD302E   lda $2E30,x
5B51  49FF     eor #$FF
5B53  6900     adc #$00
5B55  9D302E   sta $2E30,x
5B58  60       rts
5B59  A902     lda #$02
5B5B  8009     bra $5B66
5B5D  A901     lda #$01
5B5F  8005     bra $5B66
5B61  62       cla
5B62  8002     bra $5B66
5B64  A905     lda #$05
5B66  854F     sta $4F
5B68  8D0000   sta $0000
5B6B  60       rts
5B6C  62       cla
5B6D  48       pha
5B6E  44F4     bsr $5B64
5B70  68       pla
5B71  8D0300   sta $0003
5B74  60       rts
5B75  20D843   jsr $43D8
5B78  9C0204   stz $0402
5B7B  9C0304   stz $0403
5B7E  9C0404   stz $0404
5B81  9C0504   stz $0405
5B84  9C0204   stz $0402
5B87  A901     lda #$01
5B89  8D0304   sta $0403
5B8C  9C0404   stz $0404
5B8F  9C0504   stz $0405
5B92  F30340D0260300 tai $4003,$26D0,$0003
5B99  F30340F3264D16 tai $4003,$26F3,$164D
5BA0  20D843   jsr $43D8
5BA3  A904     lda #$04
5BA5  8539     sta $39
5BA7  6445     stz $45
5BA9  644C     stz $4C
5BAB  F30340003EEE00 tai $4003,$3E00,$00EE
5BB2  206C5B   jsr $5B6C
5BB5  20615B   jsr $5B61
5BB8  1300     st1 #$00
5BBA  2300     st2 #$00
5BBC  20595B   jsr $5B59
5BBF  82       clx
5BC0  A00F     ldy #$0F
5BC2  1310     st1 #$10
5BC4  2301     st2 #$01
5BC6  E8       inx
5BC7  D0F9     bne $5BC2
5BC9  88       dey
5BCA  10F6     bpl $5BC2
5BCC  60       rts
5D3E  ADD226   lda $26D2
5D41  C901     cmp #$01
5D43  F051     beq $5D96
5D45  A901     lda #$01
5D47  8D4427   sta $2744
5D4A  A901     lda #$01
5D4C  8D0127   sta $2701
5D4F  9C0227   stz $2702
5D52  AD963E   lda $3E96
5D55  8505     sta $05
5D57  AD083E   lda $3E08
5D5A  8504     sta $04
5D5C  A901     lda #$01
5D5E  8DFF26   sta $26FF
5D61  A941     lda #$41
5D63  8D0027   sta $2700
5D66  A901     lda #$01
5D68  8506     sta $06
5D6A  20E75D   jsr $5DE7
5D6D  A9FF     lda #$FF
5D6F  8D0127   sta $2701
5D72  A9FF     lda #$FF
5D74  8D0227   sta $2702
5D77  AD963E   lda $3E96
5D7A  8505     sta $05
5D7C  AD083E   lda $3E08
5D7F  8504     sta $04
5D81  A900     lda #$00
5D83  8DFF26   sta $26FF
5D86  A941     lda #$41
5D88  8D0027   sta $2700
5D8B  A901     lda #$01
5D8D  8506     sta $06
5D8F  20E75D   jsr $5DE7
5D92  9C4427   stz $2744
5D95  60       rts
5D96  A901     lda #$01
5D98  8D4427   sta $2744
5D9B  A901     lda #$01
5D9D  8D0127   sta $2701
5DA0  9C0227   stz $2702
5DA3  AD8E3E   lda $3E8E
5DA6  8505     sta $05
5DA8  AD203E   lda $3E20
5DAB  8504     sta $04
5DAD  A901     lda #$01
5DAF  8DFF26   sta $26FF
5DB2  A941     lda #$41
5DB4  8D0027   sta $2700
5DB7  A901     lda #$01
5DB9  8506     sta $06
5DBB  20E75D   jsr $5DE7
5DBE  A9FF     lda #$FF
5DC0  8D0127   sta $2701
5DC3  A9FF     lda #$FF
5DC5  8D0227   sta $2702
5DC8  AD8E3E   lda $3E8E
5DCB  8505     sta $05
5DCD  AD203E   lda $3E20
5DD0  8504     sta $04
5DD2  A900     lda #$00
5DD4  8DFF26   sta $26FF
5DD7  A941     lda #$41
5DD9  8D0027   sta $2700
5DDC  A901     lda #$01
5DDE  8506     sta $06
5DE0  20E75D   jsr $5DE7
5DE3  9C4427   stz $2744
5DE6  60       rts
5DE7  ADD026   lda $26D0
5DEA  8510     sta $10
5DEC  ADD126   lda $26D1
5DEF  8511     sta $11
5DF1  C2       cly
5DF2  B110     lda ($10),y
5DF4  8512     sta $12
5DF6  C8       iny
5DF7  B110     lda ($10),y
5DF9  8513     sta $13
5DFB  AD0127   lda $2701
5DFE  3005     bmi $5E05
5E00  20CC5E   jsr $5ECC
5E03  8003     bra $5E08
5E05  20445F   jsr $5F44
5E08  A504     lda $04
5E0A  18       clc
5E0B  6D0127   adc $2701
5E0E  8504     sta $04
5E10  A505     lda $05
5E12  6D0227   adc $2702
5E15  8505     sta $05
5E17  AD0027   lda $2700
5E1A  38       sec
5E1B  E901     sbc #$01
5E1D  8D0027   sta $2700
5E20  ADFF26   lda $26FF
5E23  E900     sbc #$00
5E25  8DFF26   sta $26FF
5E28  0D0027   ora $2700
5E2B  D0BA     bne $5DE7
5E2D  60       rts
5E2E  ADD026   lda $26D0
5E31  8510     sta $10
5E33  ADD126   lda $26D1
5E36  8511     sta $11
5E38  C2       cly
5E39  B110     lda ($10),y
5E3B  8512     sta $12
5E3D  C8       iny
5E3E  B110     lda ($10),y
5E40  8513     sta $13
5E42  ADD226   lda $26D2
5E45  C901     cmp #$01
5E47  F00D     beq $5E56
5E49  AD9E3E   lda $3E9E
5E4C  F015     beq $5E63
5E4E  1003     bpl $5E53
5E50  4C7B5E   jmp $5E7B
5E53  4C645E   jmp $5E64
5E56  ADA63E   lda $3EA6
5E59  F008     beq $5E63
5E5B  3003     bmi $5E60
5E5D  4C985E   jmp $5E98
5E60  4CAF5E   jmp $5EAF
5E63  60       rts
5E64  AD083E   lda $3E08
5E67  18       clc
5E68  6940     adc #$40
5E6A  8504     sta $04
5E6C  AD963E   lda $3E96
5E6F  6901     adc #$01
5E71  8505     sta $05
5E73  AD9E3E   lda $3E9E
5E76  8506     sta $06
5E78  4CCC5E   jmp $5ECC
5E7B  AD083E   lda $3E08
5E7E  18       clc
5E7F  69C0     adc #$C0
5E81  8504     sta $04
5E83  AD963E   lda $3E96
5E86  69FF     adc #$FF
5E88  300D     bmi $5E97
5E8A  8505     sta $05
5E8C  AD9E3E   lda $3E9E
5E8F  49FF     eor #$FF
5E91  1A       inc a
5E92  8506     sta $06
5E94  4C445F   jmp $5F44
5E97  60       rts
5E98  AD203E   lda $3E20
5E9B  18       clc
5E9C  6940     adc #$40
5E9E  8504     sta $04
5EA0  AD8E3E   lda $3E8E
5EA3  6901     adc #$01
5EA5  8505     sta $05
5EA7  ADA63E   lda $3EA6
5EAA  8506     sta $06
5EAC  4CCC5E   jmp $5ECC
5EAF  AD203E   lda $3E20
5EB2  18       clc
5EB3  69C0     adc #$C0
5EB5  8504     sta $04
5EB7  AD8E3E   lda $3E8E
5EBA  69FF     adc #$FF
5EBC  300D     bmi $5ECB
5EBE  8505     sta $05
5EC0  ADA63E   lda $3EA6
5EC3  49FF     eor #$FF
5EC5  1A       inc a
5EC6  8506     sta $06
5EC8  4C445F   jmp $5F44
5ECB  60       rts
5ECC  20235F   jsr $5F23
5ECF  A515     lda $15
5ED1  D028     bne $5EFB
5ED3  A514     lda $14
5ED5  C506     cmp $06
5ED7  B022     bcs $5EFB
5ED9  20A55F   jsr $5FA5
5EDC  A510     lda $10
5EDE  18       clc
5EDF  690A     adc #$0A
5EE1  8510     sta $10
5EE3  8DD026   sta $26D0
5EE6  A511     lda $11
5EE8  6900     adc #$00
5EEA  8511     sta $11
5EEC  8DD126   sta $26D1
5EEF  C2       cly
5EF0  B110     lda ($10),y
5EF2  8512     sta $12
5EF4  C8       iny
5EF5  B110     lda ($10),y
5EF7  8513     sta $13
5EF9  80D1     bra $5ECC
5EFB  A513     lda $13
5EFD  2512     and $12
5EFF  C9FF     cmp #$FF
5F01  F010     beq $5F13
5F03  A513     lda $13
5F05  C505     cmp $05
5F07  90D3     bcc $5EDC
5F09  D017     bne $5F22
5F0B  A512     lda $12
5F0D  C504     cmp $04
5F0F  B011     bcs $5F22
5F11  80C9     bra $5EDC
5F13  A510     lda $10
5F15  38       sec
5F16  E90A     sbc #$0A
5F18  8DD026   sta $26D0
5F1B  A511     lda $11
5F1D  E900     sbc #$00
5F1F  8DD126   sta $26D1
5F22  60       rts
5F23  A512     lda $12
5F25  38       sec
5F26  E504     sbc $04
5F28  8514     sta $14
5F2A  A513     lda $13
5F2C  E505     sbc $05
5F2E  8515     sta $15
5F30  B011     bcs $5F43
5F32  A514     lda $14
5F34  49FF     eor #$FF
5F36  18       clc
5F37  6901     adc #$01
5F39  8514     sta $14
5F3B  A515     lda $15
5F3D  49FF     eor #$FF
5F3F  6900     adc #$00
5F41  8515     sta $15
5F43  60       rts
5F44  20235F   jsr $5F23
5F47  A515     lda $15
5F49  D032     bne $5F7D
5F4B  A514     lda $14
5F4D  C506     cmp $06
5F4F  B02C     bcs $5F7D
5F51  20A55F   jsr $5FA5
5F54  A510     lda $10
5F56  38       sec
5F57  E90A     sbc #$0A
5F59  8510     sta $10
5F5B  A511     lda $11
5F5D  E900     sbc #$00
5F5F  8511     sta $11
5F61  A001     ldy #$01
5F63  B110     lda ($10),y
5F65  303D     bmi $5FA4
5F67  A510     lda $10
5F69  8DD026   sta $26D0
5F6C  A511     lda $11
5F6E  8DD126   sta $26D1
5F71  C2       cly
5F72  B110     lda ($10),y
5F74  8512     sta $12
5F76  C8       iny
5F77  B110     lda ($10),y
5F79  8513     sta $13
5F7B  80C7     bra $5F44
5F7D  A513     lda $13
5F7F  2512     and $12
5F81  C9FF     cmp #$FF
5F83  F010     beq $5F95
5F85  A513     lda $13
5F87  C505     cmp $05
5F89  9019     bcc $5FA4
5F8B  D0C7     bne $5F54
5F8D  A512     lda $12
5F8F  C504     cmp $04
5F91  9011     bcc $5FA4
5F93  80BF     bra $5F54
5F95  A510     lda $10
5F97  18       clc
5F98  690A     adc #$0A
5F9A  8DD026   sta $26D0
5F9D  A511     lda $11
5F9F  6900     adc #$00
5FA1  8DD126   sta $26D1
5FA4  60       rts
5FA5  A002     ldy #$02
5FA7  B110     lda ($10),y
5FA9  8508     sta $08
5FAB  C8       iny
5FAC  B110     lda ($10),y
5FAE  8509     sta $09
5FB0  C8       iny
5FB1  B110     lda ($10),y
5FB3  850A     sta $0A
5FB5  C8       iny
5FB6  B110     lda ($10),y
5FB8  18       clc
5FB9  6910     adc #$10
5FBB  850B     sta $0B
5FBD  C8       iny
5FBE  B110     lda ($10),y
5FC0  850C     sta $0C
5FC2  C8       iny
5FC3  B110     lda ($10),y
5FC5  850D     sta $0D
5FC7  C8       iny
5FC8  B110     lda ($10),y
5FCA  850E     sta $0E
5FCC  C8       iny
5FCD  B110     lda ($10),y
5FCF  850F     sta $0F
5FD1  207540   jsr $4075
5FD4  39D326   and $26D3,y
5FD7  F001     beq $5FDA
5FD9  60       rts
5FDA  A508     lda $08
5FDC  0A       asl a
5FDD  A8       tay
5FDE  B9EB5F   lda $5FEB,y
5FE1  8500     sta $00
5FE3  B9EC5F   lda $5FEC,y
5FE6  8501     sta $01
5FE8  6C0020   jmp ($2000)
60E6  AD1280   lda $8012
60E9  8516     sta $16
60EB  AD1380   lda $8013
60EE  8517     sta $17
60F0  BD5027   lda $2750,x
60F3  3A       dec a
60F4  0A       asl a
60F5  A8       tay
60F6  B116     lda ($16),y
60F8  8510     sta $10
60FA  C8       iny
60FB  B116     lda ($16),y
60FD  8511     sta $11
60FF  C2       cly
6100  B110     lda ($10),y
6102  9D5833   sta $3358,x
6105  C8       iny
6106  B110     lda ($10),y
6108  9D0033   sta $3300,x
610B  C8       iny
610C  B110     lda ($10),y
610E  9DA832   sta $32A8,x
6111  C8       iny
6112  B110     lda ($10),y
6114  290F     and #$0F
6116  9D5032   sta $3250,x
6119  B110     lda ($10),y
611B  4A       lsr a
611C  4A       lsr a
611D  4A       lsr a
611E  4A       lsr a
611F  9DE02E   sta $2EE0,x
6122  C8       iny
6123  B110     lda ($10),y
6125  9D8839   sta $3988,x
6128  C8       iny
6129  B110     lda ($10),y
612B  9D383A   sta $3A38,x
612E  C8       iny
612F  B110     lda ($10),y
6131  9D102A   sta $2A10,x
6134  C8       iny
6135  B110     lda ($10),y
6137  9D682A   sta $2A68,x
613A  C8       iny
613B  B110     lda ($10),y
613D  9DF030   sta $30F0,x
6140  C8       iny
6141  B110     lda ($10),y
6143  9D4831   sta $3148,x
6146  C8       iny
6147  B110     lda ($10),y
6149  9DA031   sta $31A0,x
614C  C8       iny
614D  B110     lda ($10),y
614F  9DF831   sta $31F8,x
6152  9E3039   stz $3930,x
6155  9EE039   stz $39E0,x
6158  FEA827   inc $27A8,x
615B  60       rts
615C  995027   sta $2750,y
615F  BD702B   lda $2B70,x
6162  99702B   sta $2B70,y
6165  BDC82B   lda $2BC8,x
6168  99C82B   sta $2BC8,y
616B  BD202C   lda $2C20,x
616E  99202C   sta $2C20,y
6171  BD782C   lda $2C78,x
6174  99782C   sta $2C78,y
6177  BDD02C   lda $2CD0,x
617A  99D02C   sta $2CD0,y
617D  BD282D   lda $2D28,x
6180  99282D   sta $2D28,y
6183  60       rts
6184  02       sxy
6185  205F61   jsr $615F
6188  02       sxy
6189  60       rts
618A  BD6034   lda $3460,x
618D  D026     bne $61B5
618F  BD0834   lda $3408,x
6192  0A       asl a
6193  A8       tay
6194  B110     lda ($10),y
6196  D00A     bne $61A2
6198  9E0834   stz $3408,x
619B  9E6034   stz $3460,x
619E  18       clc
619F  A900     lda #$00
61A1  60       rts
61A2  9D6034   sta $3460,x
61A5  C8       iny
61A6  B110     lda ($10),y
61A8  9D682A   sta $2A68,x
61AB  FE0834   inc $3408,x
61AE  DE6034   dec $3460,x
61B1  38       sec
61B2  A9FF     lda #$FF
61B4  60       rts
61B5  DE6034   dec $3460,x
61B8  18       clc
61B9  A9FF     lda #$FF
61BB  60       rts
61BC  6413     stz $13
61BE  8512     sta $12
61C0  2901     and #$01
61C2  F061     beq $6225
61C4  A514     lda $14
61C6  20BD62   jsr $62BD
61C9  ADF326   lda $26F3
61CC  0DF426   ora $26F4
61CF  0DF526   ora $26F5
61D2  2910     and #$10
61D4  F016     beq $61EC
61D6  9E802D   stz $2D80,x
61D9  9ED82D   stz $2DD8,x
61DC  9E302E   stz $2E30,x
61DF  A940     lda #$40
61E1  9D882E   sta $2E88,x
61E4  A513     lda $13
61E6  0910     ora #$10
61E8  8513     sta $13
61EA  8039     bra $6225
61EC  C2       cly
61ED  B9F326   lda $26F3,y
61F0  2921     and #$21
61F2  D007     bne $61FB
61F4  C8       iny
61F5  C003     cpy #$03
61F7  90F4     bcc $61ED
61F9  802A     bra $6225
61FB  A513     lda $13
61FD  0901     ora #$01
61FF  8513     sta $13
6201  A512     lda $12
6203  2908     and #$08
6205  F01E     beq $6225
6207  B9FB26   lda $26FB,y
620A  2A       rol a
620B  2A       rol a
620C  2901     and #$01
620E  49FF     eor #$FF
6210  1A       inc a
6211  8500     sta $00
6213  BDD02C   lda $2CD0,x
6216  38       sec
6217  F9FB26   sbc $26FB,y
621A  9DD02C   sta $2CD0,x
621D  BD782C   lda $2C78,x
6220  E500     sbc $00
6222  9D782C   sta $2C78,x
6225  A512     lda $12
6227  2902     and #$02
6229  F054     beq $627F
622B  A514     lda $14
622D  1A       inc a
622E  20BD62   jsr $62BD
6231  C2       cly
6232  BD802D   lda $2D80,x
6235  3002     bmi $6239
6237  A002     ldy #$02
6239  B9F326   lda $26F3,y
623C  2902     and #$02
623E  D008     bne $6248
6240  C8       iny
6241  B9F326   lda $26F3,y
6244  2902     and #$02
6246  F037     beq $627F
6248  A513     lda $13
624A  0902     ora #$02
624C  8513     sta $13
624E  A512     lda $12
6250  2920     and #$20
6252  F02B     beq $627F
6254  C002     cpy #$02
6256  B015     bcs $626D
6258  B9F726   lda $26F7,y
625B  A8       tay
625C  BDC82B   lda $2BC8,x
625F  18       clc
6260  79C362   adc $62C3,y
6263  9DC82B   sta $2BC8,x
6266  9017     bcc $627F
6268  FE702B   inc $2B70,x
626B  8012     bra $627F
626D  BDC82B   lda $2BC8,x
6270  38       sec
6271  F9F726   sbc $26F7,y
6274  9DC82B   sta $2BC8,x
6277  BD702B   lda $2B70,x
627A  E900     sbc #$00
627C  9D702B   sta $2B70,x
627F  A513     lda $13
6281  60       rts
6282  6413     stz $13
6284  8512     sta $12
6286  2901     and #$01
6288  D003     bne $628D
628A  4C2562   jmp $6225
628D  A514     lda $14
628F  20C062   jsr $62C0
6292  4CC961   jmp $61C9
6295  6413     stz $13
6297  8512     sta $12
6299  2901     and #$01
629B  D003     bne $62A0
629D  4C2562   jmp $6225
62A0  A514     lda $14
62A2  20C062   jsr $62C0
62A5  ADF326   lda $26F3
62A8  2910     and #$10
62AA  F003     beq $62AF
62AC  4CD661   jmp $61D6
62AF  C2       cly
62B0  ADF326   lda $26F3
62B3  2901     and #$01
62B5  F003     beq $62BA
62B7  4CFB61   jmp $61FB
62BA  4C2562   jmp $6225
62BD  4C0C80   jmp $800C
62C0  4C0F80   jmp $800F
62CB  BD702B   lda $2B70,x
62CE  0D702B   ora $2B70
62D1  D00D     bne $62E0
62D3  BDC82B   lda $2BC8,x
62D6  38       sec
62D7  EDC82B   sbc $2BC8
62DA  B003     bcs $62DF
62DC  49FF     eor #$FF
62DE  1A       inc a
62DF  60       rts
62E0  A9FF     lda #$FF
62E2  60       rts
62E3  BD782C   lda $2C78,x
62E6  0D782C   ora $2C78
62E9  D00D     bne $62F8
62EB  BDD02C   lda $2CD0,x
62EE  38       sec
62EF  EDD02C   sbc $2CD0
62F2  B003     bcs $62F7
62F4  49FF     eor #$FF
62F6  1A       inc a
62F7  60       rts
62F8  A9FF     lda #$FF
62FA  60       rts
62FB  BD702B   lda $2B70,x
62FE  D00A     bne $630A
6300  BDC82B   lda $2BC8,x
6303  CDC82B   cmp $2BC8
6306  B00A     bcs $6312
6308  8002     bra $630C
630A  1006     bpl $6312
630C  A901     lda #$01
630E  8500     sta $00
6310  8002     bra $6314
6312  6400     stz $00
6314  BD782C   lda $2C78,x
6317  D00A     bne $6323
6319  BDD02C   lda $2CD0,x
631C  CDD02C   cmp $2CD0
631F  B009     bcs $632A
6321  8002     bra $6325
6323  1005     bpl $632A
6325  A500     lda $00
6327  0902     ora #$02
6329  60       rts
632A  A500     lda $00
632C  60       rts
632D  8514     sta $14
632F  204540   jsr $4045
6332  9CFF26   stz $26FF
6335  9C0027   stz $2700
6338  20ED63   jsr $63ED
633B  BD302E   lda $2E30,x
633E  3012     bmi $6352
6340  A909     lda #$09
6342  20BC61   jsr $61BC
6345  2901     and #$01
6347  F009     beq $6352
6349  EE0027   inc $2700
634C  9E302E   stz $2E30,x
634F  9E882E   stz $2E88,x
6352  A922     lda #$22
6354  20BC61   jsr $61BC
6357  2902     and #$02
6359  F007     beq $6362
635B  AD0027   lda $2700
635E  0902     ora #$02
6360  38       sec
6361  60       rts
6362  AD0027   lda $2700
6365  18       clc
6366  60       rts
6367  8514     sta $14
6369  204540   jsr $4045
636C  9CFF26   stz $26FF
636F  9C0027   stz $2700
6372  20ED63   jsr $63ED
6375  BD302E   lda $2E30,x
6378  3008     bmi $6382
637A  A909     lda #$09
637C  208262   jsr $6282
637F  8DFF26   sta $26FF
6382  A922     lda #$22
6384  20BC61   jsr $61BC
6387  2902     and #$02
6389  8D0027   sta $2700
638C  ADFF26   lda $26FF
638F  2901     and #$01
6391  F013     beq $63A6
6393  9E802D   stz $2D80,x
6396  9ED82D   stz $2DD8,x
6399  9E302E   stz $2E30,x
639C  9E882E   stz $2E88,x
639F  AD0027   lda $2700
63A2  0901     ora #$01
63A4  38       sec
63A5  60       rts
63A6  AD0027   lda $2700
63A9  18       clc
63AA  60       rts
63AB  8514     sta $14
63AD  204540   jsr $4045
63B0  9CFF26   stz $26FF
63B3  9C0027   stz $2700
63B6  20ED63   jsr $63ED
63B9  BD302E   lda $2E30,x
63BC  30C4     bmi $6382
63BE  A909     lda #$09
63C0  209562   jsr $6295
63C3  8DFF26   sta $26FF
63C6  80BA     bra $6382
63C8  A030     ldy #$30
63CA  B95027   lda $2750,y
63CD  F007     beq $63D6
63CF  C8       iny
63D0  C040     cpy #$40
63D2  90F6     bcc $63CA
63D4  18       clc
63D5  60       rts
63D6  205D40   jsr $405D
63D9  38       sec
63DA  60       rts
63DB  A020     ldy #$20
63DD  80EB     bra $63CA
63DF  A050     ldy #$50
63E1  B95027   lda $2750,y
63E4  F0F0     beq $63D6
63E6  C8       iny
63E7  C054     cpy #$54
63E9  90F6     bcc $63E1
63EB  18       clc
63EC  60       rts
63ED  BD302E   lda $2E30,x
63F0  3004     bmi $63F6
63F2  C907     cmp #$07
63F4  B00E     bcs $6404
63F6  BD882E   lda $2E88,x
63F9  18       clc
63FA  6938     adc #$38
63FC  9D882E   sta $2E88,x
63FF  9003     bcc $6404
6401  FE302E   inc $2E30,x
6404  60       rts
6405  0A       asl a
6406  0A       asl a
6407  0A       asl a
6408  0A       asl a
6409  A8       tay
640A  A504     lda $04
640C  290F     and #$0F
640E  8504     sta $04
6410  98       tya
6411  0504     ora $04
6413  8504     sta $04
6415  805F     bra $6476
6417  290F     and #$0F
6419  A8       tay
641A  A504     lda $04
641C  29F0     and #$F0
641E  80EE     bra $640E
6420  60       rts
6421  291F     and #$1F
6423  A8       tay
6424  B202     lda ($02)
6426  205065   jsr $6550
6429  200965   jsr $6509
642C  88       dey
642D  F047     beq $6476
642F  200E65   jsr $650E
6432  88       dey
6433  D0F4     bne $6429
6435  803F     bra $6476
6437  291F     and #$1F
6439  A8       tay
643A  B202     lda ($02)
643C  205065   jsr $6550
643F  200E65   jsr $650E
6442  88       dey
6443  F031     beq $6476
6445  200965   jsr $6509
6448  88       dey
6449  D0F4     bne $643F
644B  8029     bra $6476
644D  48       pha
644E  AD1480   lda $8014
6451  8508     sta $08
6453  AD1580   lda $8015
6456  8509     sta $09
6458  98       tya
6459  0A       asl a
645A  A8       tay
645B  B108     lda ($08),y
645D  8500     sta $00
645F  C8       iny
6460  B108     lda ($08),y
6462  8501     sta $01
6464  68       pla
6465  0A       asl a
6466  A8       tay
6467  B100     lda ($00),y
6469  8502     sta $02
646B  C8       iny
646C  B100     lda ($00),y
646E  8503     sta $03
6470  202D40   jsr $402D
6473  201E65   jsr $651E
6476  B202     lda ($02)
6478  205065   jsr $6550
647B  C9FF     cmp #$FF
647D  F0A1     beq $6420
647F  C9FE     cmp #$FE
6481  F05A     beq $64DD
6483  C9F0     cmp #$F0
6485  F05B     beq $64E2
6487  C9E0     cmp #$E0
6489  F032     beq $64BD
648B  C9D0     cmp #$D0
648D  B088     bcs $6417
648F  C9C0     cmp #$C0
6491  9003     bcc $6496
6493  4C0564   jmp $6405
6496  C900     cmp #$00
6498  F031     beq $64CB
649A  C920     cmp #$20
649C  9049     bcc $64E7
649E  C940     cmp #$40
64A0  9056     bcc $64F8
64A2  C960     cmp #$60
64A4  B003     bcs $64A9
64A6  4C2164   jmp $6421
64A9  C980     cmp #$80
64AB  908A     bcc $6437
64AD  293F     and #$3F
64AF  A8       tay
64B0  B202     lda ($02)
64B2  205065   jsr $6550
64B5  201365   jsr $6513
64B8  88       dey
64B9  D0F5     bne $64B0
64BB  80B9     bra $6476
64BD  B202     lda ($02)
64BF  A8       tay
64C0  205065   jsr $6550
64C3  B202     lda ($02)
64C5  8402     sty $02
64C7  8503     sta $03
64C9  80AB     bra $6476
64CB  B202     lda ($02)
64CD  8D0200   sta $0002
64D0  205065   jsr $6550
64D3  B202     lda ($02)
64D5  8D0300   sta $0003
64D8  205065   jsr $6550
64DB  8099     bra $6476
64DD  201E65   jsr $651E
64E0  8094     bra $6476
64E2  203865   jsr $6538
64E5  808F     bra $6476
64E7  291F     and #$1F
64E9  A8       tay
64EA  B202     lda ($02)
64EC  205065   jsr $6550
64EF  201365   jsr $6513
64F2  88       dey
64F3  D0FA     bne $64EF
64F5  4C7664   jmp $6476
64F8  291F     and #$1F
64FA  A8       tay
64FB  B202     lda ($02)
64FD  205065   jsr $6550
6500  200965   jsr $6509
6503  88       dey
6504  D0FA     bne $6500
6506  4C7664   jmp $6476
6509  201365   jsr $6513
650C  1A       inc a
650D  60       rts
650E  201365   jsr $6513
6511  3A       dec a
6512  60       rts
6513  48       pha
6514  8D0200   sta $0002
6517  A504     lda $04
6519  8D0300   sta $0003
651C  68       pla
651D  60       rts
651E  202140   jsr $4021
6521  B202     lda ($02)
6523  8D0200   sta $0002
6526  8500     sta $00
6528  205065   jsr $6550
652B  B202     lda ($02)
652D  8D0300   sta $0003
6530  8501     sta $01
6532  205065   jsr $6550
6535  4C2740   jmp $4027
6538  202140   jsr $4021
653B  A500     lda $00
653D  18       clc
653E  6940     adc #$40
6540  8500     sta $00
6542  8D0200   sta $0002
6545  62       cla
6546  6501     adc $01
6548  8501     sta $01
654A  8D0300   sta $0003
654D  4C2740   jmp $4027
6550  E602     inc $02
6552  D002     bne $6556
6554  E603     inc $03
6556  60       rts
6557  20AE40   jsr $40AE
655A  6D1834   adc $3418
655D  6D8034   adc $3480
6560  6DE12C   adc $2CE1
6563  ED322C   sbc $2C32
6566  EDE32C   sbc $2CE3
6569  6DDC2B   adc $2BDC
656C  6D6527   adc $2765
656F  ED6627   sbc $2766
6572  ED372C   sbc $2C37
6575  6D002D   adc $2D00
6578  6D8127   adc $2781
657B  EDFA2B   sbc $2BFA
657E  ED032D   sbc $2D03
6581  6DE02E   adc $2EE0
6584  38       sec
6585  ED902F   sbc $2F90
6588  60       rts
6589  48       pha
658A  BD702B   lda $2B70,x
658D  1D782C   ora $2C78,x
6590  D004     bne $6596
6592  68       pla
6593  4C1B40   jmp $401B
6596  68       pla
6597  60       rts
6598  9CD82D   stz $2DD8
659B  9C802D   stz $2D80
659E  60       rts
6608  ADD82D   lda $2DD8
660B  48       pha
660C  AD802D   lda $2D80
660F  48       pha
6610  AD882E   lda $2E88
6613  48       pha
6614  AD302E   lda $2E30
6617  48       pha
6618  4FC911   bbr4 $C9,$0011
661B  ADD82D   lda $2DD8
661E  18       clc
661F  65CC     adc $CC
6621  8DD82D   sta $2DD8
6624  AD802D   lda $2D80
6627  65CD     adc $CD
6629  8D802D   sta $2D80
662C  5FC91D   bbr5 $C9,$001D
662F  AD882E   lda $2E88
6632  18       clc
6633  65CE     adc $CE
6635  8D882E   sta $2E88
6638  AD302E   lda $2E30
663B  65CF     adc $CF
663D  8D302E   sta $2E30
6640  AEF63B   ldx $3BF6
6643  BD282D   lda $2D28,x
6646  8D183E   sta $3E18
6649  8D282D   sta $2D28
664C  3FC914   bbr3 $C9,$0014
664F  9C6E3E   stz $3E6E
6652  A5CF     lda $CF
6654  8D763E   sta $3E76
6657  49FF     eor #$FF
6659  1A       inc a
665A  18       clc
665B  6DD02C   adc $2CD0
665E  8DD02C   sta $2CD0
6661  8002     bra $6665
6663  441C     bsr $6681
6665  68       pla
6666  8D302E   sta $2E30
6669  68       pla
666A  8D882E   sta $2E88
666D  68       pla
666E  8D802D   sta $2D80
6671  68       pla
6672  8DD82D   sta $2DD8
6675  2006A0   jsr $A006
6678  82       clx
6679  74C9     stz $C9,x
667B  E8       inx
667C  E007     cpx #$07
667E  D0F9     bne $6679
6680  60       rts
6681  ADB63E   lda $3EB6
6684  0A       asl a
6685  AA       tax
6686  7C8966   jmp [$6689,x]
67BB  C2       cly
67BC  2021A0   jsr $A021
67BF  F041     beq $6802
67C1  ADA827   lda $27A8
67C4  C903     cmp #$03
67C6  D00A     bne $67D2
67C8  AD302E   lda $2E30
67CB  3033     bmi $6800
67CD  209865   jsr $6598
67D0  802E     bra $6800
67D2  A500     lda $00
67D4  49FF     eor #$FF
67D6  1A       inc a
67D7  18       clc
67D8  6DC82B   adc $2BC8
67DB  C910     cmp #$10
67DD  901E     bcc $67FD
67DF  AEC82B   ldx $2BC8
67E2  DA       phx
67E3  8DC82B   sta $2BC8
67E6  9C202C   stz $2C20
67E9  209865   jsr $6598
67EC  64C9     stz $C9
67EE  A001     ldy #$01
67F0  2021A0   jsr $A021
67F3  FA       plx
67F4  29FF     and #$FF
67F6  F008     beq $6800
67F8  8EC82B   stx $2BC8
67FB  8003     bra $6800
67FD  20066A   jsr $6A06
6800  A901     lda #$01
6802  60       rts
68AF  A9F0     lda #$F0
68B1  8DC82B   sta $2BC8
68B4  9C202C   stz $2C20
68B7  60       rts
68F5  A001     ldy #$01
68F7  2021A0   jsr $A021
68FA  F041     beq $693D
68FC  ADA827   lda $27A8
68FF  C903     cmp #$03
6901  D00A     bne $690D
6903  AD302E   lda $2E30
6906  3033     bmi $693B
6908  209865   jsr $6598
690B  802E     bra $693B
690D  A600     ldx $00
690F  BD3E69   lda $693E,x
6912  18       clc
6913  6DC82B   adc $2BC8
6916  C9F0     cmp #$F0
6918  B01E     bcs $6938
691A  AEC82B   ldx $2BC8
691D  DA       phx
691E  8DC82B   sta $2BC8
6921  9C202C   stz $2C20
6924  209865   jsr $6598
6927  64C9     stz $C9
6929  A000     ldy #$00
692B  2021A0   jsr $A021
692E  FA       plx
692F  29FF     and #$FF
6931  F008     beq $693B
6933  8EC82B   stx $2BC8
6936  8003     bra $693B
6938  20AF68   jsr $68AF
693B  A901     lda #$01
693D  60       rts
6A06  A910     lda #$10
6A08  8DC82B   sta $2BC8
6A0B  9C202C   stz $2C20
6A0E  60       rts
6C24  A99A     lda #$9A
6C26  85F8     sta $F8
6C28  A96C     lda #$6C
6C2A  85F9     sta $F9
6C2C  A900     lda #$00
6C2E  85FA     sta $FA
6C30  A97D     lda #$7D
6C32  85FB     sta $FB
6C34  A990     lda #$90
6C36  85FC     sta $FC
6C38  A900     lda #$00
6C3A  85FD     sta $FD
6C3C  64FE     stz $FE
6C3E  64FF     stz $FF
6C40  4C4EE0   jmp $E04E
6C43  A99A     lda #$9A
6C45  85F8     sta $F8
6C47  A96C     lda #$6C
6C49  85F9     sta $F9
6C4B  2054E0   jsr $E054
6C4E  204BE0   jsr $E04B
6C51  A5FC     lda $FC
6C53  C992     cmp #$92
6C55  A5FD     lda $FD
6C57  E900     sbc #$00
6C59  302E     bmi $6C89
6C5B  A99A     lda #$9A
6C5D  85F8     sta $F8
6C5F  A96C     lda #$6C
6C61  85F9     sta $F9
6C63  A900     lda #$00
6C65  85FA     sta $FA
6C67  A97D     lda #$7D
6C69  85FB     sta $FB
6C6B  A990     lda #$90
6C6D  85FC     sta $FC
6C6F  A900     lda #$00
6C71  85FD     sta $FD
6C73  64FE     stz $FE
6C75  64FF     stz $FF
6C77  4C51E0   jmp $E051
6C7A  A98F     lda #$8F
6C7C  85F8     sta $F8
6C7E  A96C     lda #$6C
6C80  85F9     sta $F9
6C82  2048E0   jsr $E048
6C85  AA       tax
6C86  D004     bne $6C8C
6C88  60       rts
6C89  A901     lda #$01
6C8B  60       rts
6C8C  4C8C6C   jmp $6C8C
6E42  60       rts
6E43  C5A2     cmp $A2
6E45  F0FB     beq $6E42
6E47  85A2     sta $A2
6E49  48       pha
6E4A  2042E0   jsr $E042
6E4D  2045E0   jsr $E045
6E50  A8       tay
6E51  D0FA     bne $6E4D
6E53  68       pla
6E54  0A       asl a
6E55  0A       asl a
6E56  18       clc
6E57  6935     adc #$35
6E59  8500     sta $00
6E5B  62       cla
6E5C  6971     adc #$71
6E5E  8501     sta $01
6E60  62       cla
6E61  201BE0   jsr $E01B
6E64  C900     cmp #$00
6E66  F009     beq $6E71
6E68  2018E0   jsr $E018
6E6B  C900     cmp #$00
6E6D  D0F9     bne $6E68
6E6F  80EF     bra $6E60
6E71  C2       cly
6E72  B100     lda ($00),y
6E74  38       sec
6E75  E932     sbc #$32
6E77  85FE     sta $FE
6E79  C8       iny
6E7A  B100     lda ($00),y
6E7C  E90F     sbc #$0F
6E7E  85FD     sta $FD
6E80  C8       iny
6E81  B100     lda ($00),y
6E83  E900     sbc #$00
6E85  85FC     sta $FC
6E87  C8       iny
6E88  B100     lda ($00),y
6E8A  85F8     sta $F8
6E8C  64FF     stz $FF
6E8E  64FB     stz $FB
6E90  64FA     stz $FA
6E92  2033E0   jsr $E033
6E95  C900     cmp #$00
6E97  D0C7     bne $6E60
6E99  9CFB25   stz $25FB
6E9C  60       rts
6EEB  85F8     sta $F8
6EED  A905     lda #$05
6EEF  85FA     sta $FA
6EF1  20BDE0   jsr $E0BD
6EF4  A90D     lda #$0D
6EF6  18       clc
6EF7  65FC     adc $FC
6EF9  8500     sta $00
6EFB  A972     lda #$72
6EFD  65FD     adc $FD
6EFF  8501     sta $01
6F01  62       cla
6F02  201BE0   jsr $E01B
6F05  C900     cmp #$00
6F07  F009     beq $6F12
6F09  2018E0   jsr $E018
6F0C  C900     cmp #$00
6F0E  D0F9     bne $6F09
6F10  80EF     bra $6F01
6F12  C2       cly
6F13  B100     lda ($00),y
6F15  38       sec
6F16  E932     sbc #$32
6F18  85FE     sta $FE
6F1A  C8       iny
6F1B  B100     lda ($00),y
6F1D  E90F     sbc #$0F
6F1F  85FD     sta $FD
6F21  C8       iny
6F22  B100     lda ($00),y
6F24  E900     sbc #$00
6F26  85FC     sta $FC
6F28  C8       iny
6F29  B100     lda ($00),y
6F2B  85F8     sta $F8
6F2D  C8       iny
6F2E  B100     lda ($00),y
6F30  85F9     sta $F9
6F32  64FA     stz $FA
6F34  A90E     lda #$0E
6F36  85FF     sta $FF
6F38  203FE0   jsr $E03F
6F3B  C900     cmp #$00
6F3D  D0C2     bne $6F01
6F3F  9CFB25   stz $25FB
6F42  60       rts
6F43  DA       phx
6F44  5A       phy
6F45  A242     ldx #$42
6F47  8618     stx $18
6F49  8008     bra $6F53
6F4B  A59C     lda $9C
6F4D  DA       phx
6F4E  5A       phy
6F4F  A241     ldx #$41
6F51  8618     stx $18
6F53  859C     sta $9C
6F55  85F8     sta $F8
6F57  A906     lda #$06
6F59  85FA     sta $FA
6F5B  20BDE0   jsr $E0BD
6F5E  A950     lda #$50
6F60  18       clc
6F61  65FC     adc $FC
6F63  8500     sta $00
6F65  A972     lda #$72
6F67  65FD     adc $FD
6F69  8501     sta $01
6F6B  A940     lda #$40
6F6D  85FB     sta $FB
6F6F  B200     lda ($00)
6F71  85F8     sta $F8
6F73  A001     ldy #$01
6F75  B100     lda ($00),y
6F77  85F9     sta $F9
6F79  C8       iny
6F7A  B100     lda ($00),y
6F7C  85FA     sta $FA
6F7E  A518     lda $18
6F80  85FF     sta $FF
6F82  C8       iny
6F83  B100     lda ($00),y
6F85  85FC     sta $FC
6F87  C8       iny
6F88  B100     lda ($00),y
6F8A  85FD     sta $FD
6F8C  C8       iny
6F8D  B100     lda ($00),y
6F8F  85FE     sta $FE
6F91  2012E0   jsr $E012
6F94  AA       tax
6F95  D0D4     bne $6F6B
6F97  62       cla
6F98  202DE0   jsr $E02D
6F9B  7A       ply
6F9C  FA       plx
6F9D  60       rts
6FD8  8502     sta $02
6FDA  85F8     sta $F8
6FDC  A905     lda #$05
6FDE  85FA     sta $FA
6FE0  20BDE0   jsr $E0BD
6FE3  A9D1     lda #$D1
6FE5  18       clc
6FE6  65FC     adc $FC
6FE8  8500     sta $00
6FEA  A974     lda #$74
6FEC  65FD     adc $FD
6FEE  8501     sta $01
6FF0  A003     ldy #$03
6FF2  B100     lda ($00),y
6FF4  8508     sta $08
6FF6  C8       iny
6FF7  B100     lda ($00),y
6FF9  8509     sta $09
6FFB  B208     lda ($08)
6FFD  302C     bmi $702B