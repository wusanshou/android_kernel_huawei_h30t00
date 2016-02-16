/*******************************************************************************************/


/*******************************************************************************************/

#include <linux/videodev2.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/xlog.h>
#include <asm/atomic.h>
#include <asm/system.h>

#include "kd_camera_hw.h"
#include "kd_imgsensor.h"
#include "kd_imgsensor_define.h"
#include "kd_imgsensor_errcode.h"

#include "s5k4e1gamipiraw_Sensor.h"
#include "s5k4e1gamipiraw_Camera_Sensor_para.h"
#include "s5k4e1gamipiraw_CameraCustomized.h"

//****************************** +FUNCTION DECLARATION ******************************//
extern int iReadReg(u16 a_u2Addr , u8 * a_puBuff , u16 i2cId);
extern int iWriteReg(u16 a_u2Addr , u32 a_u4Data , u32 a_u4Bytes , u16 i2cId);
static UINT32 S5K4E1GASetFrameRate_Limit(UINT16 u2FrameRate);
#define S5K4E1GA_TEST_PATTERN_CHECKSUM (0x854e0ecf)
//****************************** -FUNCTION DECLARATION ******************************//



//******************************** +MACRO DEFINITION ********************************//
#define S5K4E1GA_DEBUG
#ifdef S5K4E1GA_DEBUG
#define LOG_TAG (__FUNCTION__)
#define SENSORDB(fmt,arg...) xlog_printk(ANDROID_LOG_DEBUG , LOG_TAG, fmt, ##arg)  
//#define LOG_TAG "[SENSOR_DRV]"
//#define SENSORDB(fmt,arg...) printk(LOG_TAG "%s: " fmt "\n", __FUNCTION__ ,##arg)
#else
#define SENSORDB(fmt,arg...)  
#endif
#define S5K4E1GA_1M_SIZE_PREVIEW
#define S5K4E1GA_write_cmos_sensor(addr, para) iWriteReg((u16) addr , (u32) para , 1, S5K4E1GAMIPI_WRITE_ID)

#define SHUTTER_TO_FRAME_LENGTH_MARGIN (8)
#define SHUTTER_TO_FRAME_LENGTH(shutter) ((shutter)+SHUTTER_TO_FRAME_LENGTH_MARGIN)

//frame rate is 100 based
#define TO_FRAME_RATE(pclk,line_length,frame_length) \
								( (pclk)/(line_length)*100/(frame_length) )
//frame rate is 100 based
#define FRAME_RATE_LEFT_SHIFT_PERCENTAGE(current_fps,target_fps) \ 
								( ((current_fps)-(target_fps))*10000/(current_fps) )
//frame rate is 100 based
#define FRAME_RATE_RIGHT_SHIFT_PERCENTAGE(current_fps,target_fps) \ 
								( ((target_fps)-(current_fps))*10000/(current_fps) )

//percentage is 10000 based
//#define FRAME_RATE_LEFT_SHIFT_DELTA_FRAMELENGTH(current_frame_length,percentage) \  
//								( ((current_frame_length)*percentage)/(10000-percentage) )
						
//#define FRAME_RATE_RIGHT_SHIFT_DELTA_FRAMELENGTH(current_frame_length,percentage) \
//								( ((current_frame_length)*percentage)/(10000+percentage) )

//frame rate is 100 based, 
#define FRAME_RATE_LEFT_SHIFT_DELTA_FRAMELENGTH(current_frame_length,current_fps,target_fps) \ 
								( ((current_frame_length)*FRAME_RATE_LEFT_SHIFT_PERCENTAGE(current_fps,target_fps)) \
												/(10000-FRAME_RATE_LEFT_SHIFT_PERCENTAGE(current_fps,target_fps)) )												
#define FRAME_RATE_RIGHT_SHIFT_DELTA_FRAMELENGTH(current_frame_length,current_fps,target_fps) \ 
								( ((current_frame_length)*FRAME_RATE_RIGHT_SHIFT_PERCENTAGE(current_fps,target_fps)) \
												/(10000-FRAME_RATE_RIGHT_SHIFT_PERCENTAGE(current_fps,target_fps)) )
//******************************** -MACRO DEFINITION ********************************//



//******************************* +VARIABLE DEFINITION ******************************//
static DEFINE_SPINLOCK(s5k4e1gamipiraw_drv_lock);

MSDK_SENSOR_CONFIG_STRUCT S5K4E1GASensorConfigData;

kal_uint32 S5K4E1GA_FAC_SENSOR_REG;
kal_uint16 test_pattern = 0;
static MSDK_SCENARIO_ID_ENUM s_CurrentScenarioId = MSDK_SCENARIO_ID_CAMERA_PREVIEW;

/* FIXME: old factors and DIDNOT use now. s*/
SENSOR_REG_STRUCT S5K4E1GASensorCCT[]=CAMERA_SENSOR_CCT_DEFAULT_VALUE;
SENSOR_REG_STRUCT S5K4E1GASensorReg[ENGINEER_END]=CAMERA_SENSOR_REG_DEFAULT_VALUE;
/* FIXME: old factors and DIDNOT use now. e*/
static S5K4E1GA_PARA_STRUCT s5k4e1ga;
//******************************* -VARIABLE DEFINITION ******************************//


kal_uint16 S5K4E1GA_read_cmos_sensor(kal_uint32 addr)
{
kal_uint16 get_byte=0;
    iReadReg((u16) addr ,(u8*)&get_byte,S5K4E1GAMIPI_WRITE_ID);
    return get_byte;
}

#define Sleep(ms) mdelay(ms)

void S5K4E1GA_write_shutter(kal_uint16 shutter)
{
    kal_uint16 iExp = shutter;
    kal_uint16 S5K4E1GA_g_iExtra_ExpLines = 0 ;
	
	kal_uint16 max_shutter = 0;
	kal_uint16 extra_lines = 0;
	kal_uint16 line_length = 0,now_framerate=30, framerate=30;
	kal_uint32 frame_length = 0;
	unsigned long flags;
	// Max coarse integration time is Frame Length - 8
	// Min coarse integration time is 2.
	if(s5k4e1ga.S5K4E1GAAutoFlickerMode == KAL_TRUE)
	{
		if ( SENSOR_MODE_PREVIEW == s5k4e1ga.sensorMode )  //(g_iS5K4E1GA_Mode == S5K4E1GA_MODE_PREVIEW)	//SXGA size output
		{
			max_shutter = S5K4E1GA_PV_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines ; //992
		}
		else				//QSXGA size output
		{
			max_shutter = S5K4E1GA_FULL_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines ; //1972
		}
		
		if (shutter < 3)
			shutter = 3;

		if (shutter > max_shutter)
			extra_lines = shutter - max_shutter + 8;
		else
			extra_lines = 0;

		if ( SENSOR_MODE_PREVIEW == s5k4e1ga.sensorMode )	//SXGA size output
		{
			line_length = S5K4E1GA_PV_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels; 
			frame_length = S5K4E1GA_PV_PERIOD_LINE_NUMS+ s5k4e1ga.DummyLines + extra_lines ; 
			now_framerate = s5k4e1ga.pvPclk * 100000/(S5K4E1GA_PV_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)*10/frame_length;
		}
		else				//QSXGA size output
		{
			line_length = S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels; 
			frame_length = S5K4E1GA_FULL_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines + extra_lines ; 
			now_framerate = s5k4e1ga.capPclk * 100000/(S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)*10/frame_length;
		}		
		framerate = now_framerate;
		SENSORDB("frame_length=%d,now_framerate=%d\n",frame_length,now_framerate);
		
		if( now_framerate == 300)
		{	
			if (s5k4e1ga.sensorMode <= SENSOR_MODE_SMALL_SIZE_END)	
			{
				frame_length = (s5k4e1ga.pvPclk * 100000) /(S5K4E1GA_PV_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)/296*10;
				framerate = (s5k4e1ga.pvPclk * 100000) /(S5K4E1GA_PV_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)*10/frame_length;
			}
			else if(s5k4e1ga.sensorMode <= SENSOR_MODE_FULL_SIZE_END)
			{
				frame_length = (s5k4e1ga.capPclk * 100000) /(S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)/296*10;
				framerate = (s5k4e1ga.capPclk * 100000) /(S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)*10/frame_length;
			}
		}
		else if ( now_framerate == 150)
		{
			if (s5k4e1ga.sensorMode <= SENSOR_MODE_SMALL_SIZE_END)	
			{
				frame_length = (s5k4e1ga.pvPclk * 100000) /(S5K4E1GA_PV_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)/148*10;
				framerate = (s5k4e1ga.pvPclk * 100000) /(S5K4E1GA_PV_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)*10/frame_length;
			}
			else if(s5k4e1ga.sensorMode <= SENSOR_MODE_FULL_SIZE_END)
			{
				frame_length = (s5k4e1ga.capPclk * 100000) /(S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)/148*10;
				framerate = (s5k4e1ga.capPclk * 100000) /(S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)*10/frame_length;
			}
		}
		
		SENSORDB("frame_length=%d,now_framerate=%d\n",frame_length,framerate);
	
		if (shutter < 3)
			shutter = 3;

		if (shutter > max_shutter)
			extra_lines = shutter - max_shutter + 8;
		else
			extra_lines = 0;
			
	}
	else{
		if ( SENSOR_MODE_PREVIEW == s5k4e1ga.sensorMode )  //(g_iS5K4E1GA_Mode == S5K4E1GA_MODE_PREVIEW)	//SXGA size output
		{
			max_shutter = S5K4E1GA_PV_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines ; //992
		}
		else				//QSXGA size output
		{
			max_shutter = S5K4E1GA_FULL_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines ; //1972
		}
	
		if (shutter < 3)
			shutter = 3;

		if (shutter > max_shutter)
			extra_lines = shutter - max_shutter + 8;
		else
			extra_lines = 0;

		if ( SENSOR_MODE_PREVIEW == s5k4e1ga.sensorMode )	//SXGA size output
		{
			line_length = S5K4E1GA_PV_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels; 
			frame_length = S5K4E1GA_PV_PERIOD_LINE_NUMS+ s5k4e1ga.DummyLines + extra_lines ; 
		}
		else				//QSXGA size output
		{
			line_length = S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels; 
			frame_length = S5K4E1GA_FULL_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines + extra_lines ; 
		}
	}
	
	ASSERT(line_length < S5K4E1GA_MAX_LINE_LENGTH);		//0xCCCC
	ASSERT(frame_length < S5K4E1GA_MAX_FRAME_LENGTH); 	//0xFFFF

	S5K4E1GA_write_cmos_sensor(0x0104, 0x01);	//Grouped parameter hold
	//Set total frame length
	S5K4E1GA_write_cmos_sensor(0x0340, (frame_length >> 8) & 0xFF);
	S5K4E1GA_write_cmos_sensor(0x0341, frame_length & 0xFF);
	spin_lock_irqsave(&s5k4e1gamipiraw_drv_lock,flags);
	s5k4e1ga.maxExposureLines = frame_length - 4;
	spin_unlock_irqrestore(&s5k4e1gamipiraw_drv_lock,flags);

	//Set total line length
	//S5K4E1GA_write_cmos_sensor(0x0342, (line_length >> 8) & 0xFF);
	//S5K4E1GA_write_cmos_sensor(0x0343, line_length & 0xFF);

	//Set shutter (Coarse integration time, uint: lines.)
	S5K4E1GA_write_cmos_sensor(0x0202, (shutter >> 8) & 0xFF);
	S5K4E1GA_write_cmos_sensor(0x0203, shutter & 0xFF);
	
	S5K4E1GA_write_cmos_sensor(0x0104, 0x00);	//Grouped parameter release
	
	SENSORDB("shutter=%d,extra_lines=%d,line_length=%d,frame_length=%d\n", shutter, extra_lines, line_length, frame_length);
}   /* write_S5K4E1GA_shutter */

 
void write_S5K4E1GA_gain(kal_uint16 gain)
{
	SENSORDB("gain=%d\n", gain);
	
	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s5k4e1ga.sensorGlobalGain = gain/2;
	spin_unlock(&s5k4e1gamipiraw_drv_lock);

	S5K4E1GA_write_cmos_sensor(0x0104, 0x01);	//Grouped parameter hold
	S5K4E1GA_write_cmos_sensor(0x0204, (s5k4e1ga.sensorGlobalGain & 0xFF00) >> 8); // ANALOG_GAIN_CTRLR
	S5K4E1GA_write_cmos_sensor(0x0205, s5k4e1ga.sensorGlobalGain & 0xFF);
	S5K4E1GA_write_cmos_sensor(0x0104, 0x00);	//Grouped parameter release
}

/*************************************************************************
* FUNCTION
*    S5K4E1GA_SetGain
*
* DESCRIPTION
*    This function is to set global gain to sensor.
*
* PARAMETERS
*    gain : sensor global gain(base: 0x40)
*
* RETURNS
*    the actually gain set to sensor.
*
* GLOBALS AFFECTED
*
*************************************************************************/
void S5K4E1GA_SetGain(UINT16 iGain)
{
	write_S5K4E1GA_gain(iGain);
}   /*  S5K4E1GA_SetGain_SetGain  */


/*************************************************************************
* FUNCTION
*    read_S5K4E1GA_gain
*
* DESCRIPTION
*    This function is to set global gain to sensor.
*
* PARAMETERS
*    None
*
* RETURNS
*    gain : sensor global gain(base: 0x40)
*
* GLOBALS AFFECTED
*
*************************************************************************/
kal_uint16 read_S5K4E1GA_gain(void)
{
    kal_uint8  temp_reg;
	kal_uint16 sensor_gain;

	sensor_gain = ((S5K4E1GA_read_cmos_sensor(0x0204) << 8) | S5K4E1GA_read_cmos_sensor(0x0205)); // ANALOG_GAIN_CTRLR  

	SENSORDB("sensor_gain=%d\n",sensor_gain);
	
	return sensor_gain;
}  /* read_S5K4E1GA_gain */

void S5K4E1GA_camera_para_to_sensor(void)
{
    kal_uint32    i;
    for(i=0; 0xFFFFFFFF!=S5K4E1GASensorReg[i].Addr; i++)
    {
        S5K4E1GA_write_cmos_sensor(S5K4E1GASensorReg[i].Addr, S5K4E1GASensorReg[i].Para);
    }
    for(i=ENGINEER_START_ADDR; 0xFFFFFFFF!=S5K4E1GASensorReg[i].Addr; i++)
    {
        S5K4E1GA_write_cmos_sensor(S5K4E1GASensorReg[i].Addr, S5K4E1GASensorReg[i].Para);
    }
    for(i=FACTORY_START_ADDR; i<FACTORY_END_ADDR; i++)
    {
        S5K4E1GA_write_cmos_sensor(S5K4E1GASensorCCT[i].Addr, S5K4E1GASensorCCT[i].Para);
    }
}


/*************************************************************************
* FUNCTION
*    S5K4E1GA_sensor_to_camera_para
*
* DESCRIPTION
*    // update camera_para from sensor register
*
* PARAMETERS
*    None
*
* RETURNS
*    gain : sensor global gain(base: 0x40)
*
* GLOBALS AFFECTED
*
*************************************************************************/
void S5K4E1GA_sensor_to_camera_para(void)
{
    kal_uint32    i, temp_data;
    for(i=0; 0xFFFFFFFF!=S5K4E1GASensorReg[i].Addr; i++)
    {    	
        temp_data =  S5K4E1GA_read_cmos_sensor(S5K4E1GASensorReg[i].Addr);
		spin_lock(&s5k4e1gamipiraw_drv_lock);
		S5K4E1GASensorReg[i].Para = temp_data ;
		spin_unlock(&s5k4e1gamipiraw_drv_lock);
    }
    for(i=ENGINEER_START_ADDR; 0xFFFFFFFF!=S5K4E1GASensorReg[i].Addr; i++)
    {    	
        temp_data =  S5K4E1GA_read_cmos_sensor(S5K4E1GASensorReg[i].Addr);
		spin_lock(&s5k4e1gamipiraw_drv_lock);
		S5K4E1GASensorReg[i].Para = temp_data;
		spin_unlock(&s5k4e1gamipiraw_drv_lock);
    }
}

/*************************************************************************
* FUNCTION
*    S5K4E1GA_get_sensor_group_count
*
* DESCRIPTION
*    //
*
* PARAMETERS
*    None
*
* RETURNS
*    gain : sensor global gain(base: 0x40)
*
* GLOBALS AFFECTED
*
*************************************************************************/
kal_int32  S5K4E1GA_get_sensor_group_count(void)
{
    return GROUP_TOTAL_NUMS;
}

void S5K4E1GA_get_sensor_group_info(kal_uint16 group_idx, kal_int8* group_name_ptr, kal_int32* item_count_ptr)
{
   switch (group_idx)
   {
        case PRE_GAIN:
            sprintf((char *)group_name_ptr, "CCT");
            *item_count_ptr = 2;
            break;
        case CMMCLK_CURRENT:
            sprintf((char *)group_name_ptr, "CMMCLK Current");
            *item_count_ptr = 1;
            break;
        case FRAME_RATE_LIMITATION:
            sprintf((char *)group_name_ptr, "Frame Rate Limitation");
            *item_count_ptr = 2;
            break;
        case REGISTER_EDITOR:
            sprintf((char *)group_name_ptr, "Register Editor");
            *item_count_ptr = 2;
            break;
        default:
            ASSERT(0);
}
}

void S5K4E1GA_get_sensor_item_info(kal_uint16 group_idx,kal_uint16 item_idx, MSDK_SENSOR_ITEM_INFO_STRUCT* info_ptr)
{
    kal_int16 temp_reg=0;
    kal_uint16 temp_gain=0, temp_addr=0, temp_para=0;
    
    switch (group_idx)
    {
        case PRE_GAIN:
           switch (item_idx)
          {
              case 0:
                sprintf((char *)info_ptr->ItemNamePtr,"Pregain-R");
                  temp_addr = PRE_GAIN_R_INDEX;
              break;
              case 1:
                sprintf((char *)info_ptr->ItemNamePtr,"Pregain-Gr");
                  temp_addr = PRE_GAIN_Gr_INDEX;
              break;
              case 2:
                sprintf((char *)info_ptr->ItemNamePtr,"Pregain-Gb");
                  temp_addr = PRE_GAIN_Gb_INDEX;
              break;
              case 3:
                sprintf((char *)info_ptr->ItemNamePtr,"Pregain-B");
                  temp_addr = PRE_GAIN_B_INDEX;
              break;
              case 4:
                 sprintf((char *)info_ptr->ItemNamePtr,"SENSOR_BASEGAIN");
                 temp_addr = SENSOR_BASEGAIN;
              break;
              default:
                 ASSERT(0);
          }

            temp_para= S5K4E1GASensorCCT[temp_addr].Para;
			temp_gain= (temp_para/s5k4e1ga.sensorBaseGain) * 1000;

            info_ptr->ItemValue=temp_gain;
            info_ptr->IsTrueFalse=KAL_FALSE;
            info_ptr->IsReadOnly=KAL_FALSE;
            info_ptr->IsNeedRestart=KAL_FALSE;
            info_ptr->Min= S5K4E1GA_MIN_ANALOG_GAIN * 1000;
            info_ptr->Max= S5K4E1GA_MAX_ANALOG_GAIN * 1000;
            break;
        case CMMCLK_CURRENT:
            switch (item_idx)
            {
                case 0:
                    sprintf((char *)info_ptr->ItemNamePtr,"Drv Cur[2,4,6,8]mA");
                
                    //temp_reg=MT9P017SensorReg[CMMCLK_CURRENT_INDEX].Para;
                    temp_reg = ISP_DRIVING_2MA;
                    if(temp_reg==ISP_DRIVING_2MA)
                    {
                        info_ptr->ItemValue=2;
                    }
                    else if(temp_reg==ISP_DRIVING_4MA)
                    {
                        info_ptr->ItemValue=4;
                    }
                    else if(temp_reg==ISP_DRIVING_6MA)
                    {
                        info_ptr->ItemValue=6;
                    }
                    else if(temp_reg==ISP_DRIVING_8MA)
                    {
                        info_ptr->ItemValue=8;
                    }
                
                    info_ptr->IsTrueFalse=KAL_FALSE;
                    info_ptr->IsReadOnly=KAL_FALSE;
                    info_ptr->IsNeedRestart=KAL_TRUE;
                    info_ptr->Min=2;
                    info_ptr->Max=8;
                    break;
                default:
                    ASSERT(0);
            }
            break;
        case FRAME_RATE_LIMITATION:
            switch (item_idx)
            {
                case 0:
                    sprintf((char *)info_ptr->ItemNamePtr,"Max Exposure Lines");
                    info_ptr->ItemValue=    111;  //MT9P017_MAX_EXPOSURE_LINES;
                    info_ptr->IsTrueFalse=KAL_FALSE;
                    info_ptr->IsReadOnly=KAL_TRUE;
                    info_ptr->IsNeedRestart=KAL_FALSE;
                    info_ptr->Min=0;
                    info_ptr->Max=0;
                    break;
                case 1:
                    sprintf((char *)info_ptr->ItemNamePtr,"Min Frame Rate");
                    info_ptr->ItemValue=12;
                    info_ptr->IsTrueFalse=KAL_FALSE;
                    info_ptr->IsReadOnly=KAL_TRUE;
                    info_ptr->IsNeedRestart=KAL_FALSE;
                    info_ptr->Min=0;
                    info_ptr->Max=0;
                    break;
                default:
                    ASSERT(0);
            }
            break;
        case REGISTER_EDITOR:
            switch (item_idx)
            {
                case 0:
                    sprintf((char *)info_ptr->ItemNamePtr,"REG Addr.");
                    info_ptr->ItemValue=0;
                    info_ptr->IsTrueFalse=KAL_FALSE;
                    info_ptr->IsReadOnly=KAL_FALSE;
                    info_ptr->IsNeedRestart=KAL_FALSE;
                    info_ptr->Min=0;
                    info_ptr->Max=0xFFFF;
                    break;
                case 1:
                    sprintf((char *)info_ptr->ItemNamePtr,"REG Value");
                    info_ptr->ItemValue=0;
                    info_ptr->IsTrueFalse=KAL_FALSE;
                    info_ptr->IsReadOnly=KAL_FALSE;
                    info_ptr->IsNeedRestart=KAL_FALSE;
                    info_ptr->Min=0;
                    info_ptr->Max=0xFFFF;
                    break;
                default:
                ASSERT(0);
            }
            break;
        default:
            ASSERT(0);
    }
}



kal_bool S5K4E1GA_set_sensor_item_info(kal_uint16 group_idx, kal_uint16 item_idx, kal_int32 ItemValue)
{
//   kal_int16 temp_reg;
   kal_uint16  temp_gain=0,temp_addr=0, temp_para=0;

   switch (group_idx)
    {
        case PRE_GAIN:
            switch (item_idx)
            {
              case 0:
                temp_addr = PRE_GAIN_R_INDEX;
              break;
              case 1:
                temp_addr = PRE_GAIN_Gr_INDEX;
              break;
              case 2:
                temp_addr = PRE_GAIN_Gb_INDEX;
              break;
              case 3:
                temp_addr = PRE_GAIN_B_INDEX;
              break;
              case 4:
                temp_addr = SENSOR_BASEGAIN;
              break;
              default:
                 ASSERT(0);
          }

		 temp_gain=((ItemValue*BASEGAIN+500)/1000);			//+500:get closed integer value

		  if(temp_gain>=1*BASEGAIN && temp_gain<=16*BASEGAIN)
          {
             temp_para=(temp_gain * s5k4e1ga.sensorBaseGain + BASEGAIN/2)/BASEGAIN;
          }          
          else
			  ASSERT(0);
		  spin_lock(&s5k4e1gamipiraw_drv_lock);
          S5K4E1GASensorCCT[temp_addr].Para = temp_para;
		  spin_unlock(&s5k4e1gamipiraw_drv_lock);
          S5K4E1GA_write_cmos_sensor(S5K4E1GASensorCCT[temp_addr].Addr,temp_para);
			spin_lock(&s5k4e1gamipiraw_drv_lock);
           s5k4e1ga.sensorGlobalGain= read_S5K4E1GA_gain();
		   spin_unlock(&s5k4e1gamipiraw_drv_lock);

            break;
        case CMMCLK_CURRENT:
            switch (item_idx)
            {
                case 0:
                    //no need to apply this item for driving current
                    break;
                default:
                    ASSERT(0);
            }
            break;
        case FRAME_RATE_LIMITATION:
            ASSERT(0);
            break;
        case REGISTER_EDITOR:
            switch (item_idx)
            {
                case 0:
					spin_lock(&s5k4e1gamipiraw_drv_lock);
                    S5K4E1GA_FAC_SENSOR_REG=ItemValue;
					spin_unlock(&s5k4e1gamipiraw_drv_lock);
                    break;
                case 1:
                    S5K4E1GA_write_cmos_sensor(S5K4E1GA_FAC_SENSOR_REG,ItemValue);
                    break;
                default:
                    ASSERT(0);
            }
            break;
        default:
            ASSERT(0);
    }
    return KAL_TRUE;
}

//void S5K4E1GA_set_isp_driving_current(kal_uint8 current)
//{

//}

static void S5K4E1GA_SetDummy(kal_uint16 iPixels,kal_uint16 iLines)
{
	kal_uint16 line_length = 0;
	kal_uint16 frame_length = 0;
	
	SENSORDB("iPixels=%d,iLines=%d\n",iPixels,iLines);
	 
	iPixels=0; //3 //not allowed to change line period at runtime
	
	if (s5k4e1ga.sensorMode <= SENSOR_MODE_SMALL_SIZE_END)	//SXGA size output
	{
		line_length = S5K4E1GA_PV_PERIOD_PIXEL_NUMS + iPixels;
		frame_length = S5K4E1GA_PV_PERIOD_LINE_NUMS + iLines;
	}
	else if(s5k4e1ga.sensorMode <= SENSOR_MODE_FULL_SIZE_END)				//QSXGA size output
	{
		line_length = S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + iPixels;
		frame_length = S5K4E1GA_FULL_PERIOD_LINE_NUMS + iLines;
	}
	
	//if(s5k4e1ga.maxExposureLines > frame_length )
	//	return;
	
	S5K4E1GA_write_cmos_sensor(0x0104, 0x01);	//Grouped parameter hold
	//Set frame length
	S5K4E1GA_write_cmos_sensor(0x0340, (frame_length >> 8) & 0xFF);
	S5K4E1GA_write_cmos_sensor(0x0341, frame_length & 0xFF);
	//Set line length
	S5K4E1GA_write_cmos_sensor(0x0342, (line_length >> 8) & 0xFF);
	S5K4E1GA_write_cmos_sensor(0x0343, line_length & 0xFF);
	S5K4E1GA_write_cmos_sensor(0x0104, 0x00);	//Grouped parameter release	
	
	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s5k4e1ga.m_Linelength=line_length;
	s5k4e1ga.m_Framelength=frame_length;
	s5k4e1ga.DummyLines=iLines;
	spin_unlock(&s5k4e1gamipiraw_drv_lock);
}   /*  S5K4E1GA_SetDummy */


static void S5K4E1GAPreviewSetting(void)
{
	kal_uint16 u16Temp=0;
	SENSORDB("enter!");
		
	S5K4E1GA_write_cmos_sensor(0x0100, 0x00);  // SW stand-by	
	
	S5K4E1GA_write_cmos_sensor(0x0103, 0x01);  //software reset
    S5K4E1GA_write_cmos_sensor(0x3030, 0x06);  //RAW 10

	S5K4E1GA_write_cmos_sensor(0x3000,0x05);
	S5K4E1GA_write_cmos_sensor(0x3001,0x03);
    S5K4E1GA_write_cmos_sensor(0x3002,0x08);
	S5K4E1GA_write_cmos_sensor(0x3003,0x09);
	S5K4E1GA_write_cmos_sensor(0x3004,0x2E);
	S5K4E1GA_write_cmos_sensor(0x3005,0x06);
    S5K4E1GA_write_cmos_sensor(0x3006,0x34);
	S5K4E1GA_write_cmos_sensor(0x3007,0x00);
	S5K4E1GA_write_cmos_sensor(0x3008,0x3C);
	S5K4E1GA_write_cmos_sensor(0x3009,0x3C);
    S5K4E1GA_write_cmos_sensor(0x300A,0x28);
	S5K4E1GA_write_cmos_sensor(0x300B,0x04);
	S5K4E1GA_write_cmos_sensor(0x300C,0x0A);
	S5K4E1GA_write_cmos_sensor(0x300D,0x02);
	S5K4E1GA_write_cmos_sensor(0x300E,0xE8);
    S5K4E1GA_write_cmos_sensor(0x300F,0x82);
	S5K4E1GA_write_cmos_sensor(0x3010,0x00);
	S5K4E1GA_write_cmos_sensor(0x3011,0x4C);
	S5K4E1GA_write_cmos_sensor(0x3012,0x30);
    S5K4E1GA_write_cmos_sensor(0x3013,0xC0);
	S5K4E1GA_write_cmos_sensor(0x3014,0x00);
	S5K4E1GA_write_cmos_sensor(0x3015,0x00);
	S5K4E1GA_write_cmos_sensor(0x3016,0x2C);
    S5K4E1GA_write_cmos_sensor(0x3017,0x94);
	S5K4E1GA_write_cmos_sensor(0x3018,0x78);
	#if defined( S5K4E1GA_1M_SIZE_PREVIEW )
		S5K4E1GA_write_cmos_sensor(0x301B,0x83);///////////////////////// NOT THE SAME WHEN CAPTURE -- need to check
	#else
		S5K4E1GA_write_cmos_sensor(0x301B,0x75);
	#endif
	//S5K4E1GA_write_cmos_sensor(0x301C,0x04);
	S5K4E1GA_write_cmos_sensor(0x301D,0xD4);
	//0x301e?
	
    S5K4E1GA_write_cmos_sensor(0x3021,0x02);
	S5K4E1GA_write_cmos_sensor(0x3022,0x24);
	S5K4E1GA_write_cmos_sensor(0x3024,0x40);
	S5K4E1GA_write_cmos_sensor(0x3027,0x08);
	
    S5K4E1GA_write_cmos_sensor(0x3029,0xC6); //0x06  ERIC
    S5K4E1GA_write_cmos_sensor(0x30BC,0xA8);// 0XB0/////////////////////////////
	S5K4E1GA_write_cmos_sensor(0x302B,0x01); //0x00  ERIC

	S5K4E1GA_write_cmos_sensor(0x301C,0x04);
	S5K4E1GA_write_cmos_sensor(0x30D8,0x3F);
	// ADLC setting ...
    S5K4E1GA_write_cmos_sensor(0x3070,0x5F);
	S5K4E1GA_write_cmos_sensor(0x3071,0x00);
	S5K4E1GA_write_cmos_sensor(0x3080,0x04);
	S5K4E1GA_write_cmos_sensor(0x3081,0x38);
	// MIPI setting
	S5K4E1GA_write_cmos_sensor(0x30BD,0x00);//SEL_CCP[0]
    S5K4E1GA_write_cmos_sensor(0x3084,0x15);//SYNC Mode
	S5K4E1GA_write_cmos_sensor(0x30BE,0x1A);//M_PCLKDIV_AUTO[4], M_DIV_PCLK[3:0]
	S5K4E1GA_write_cmos_sensor(0x30BF,0xAB);
	
	S5K4E1GA_write_cmos_sensor(0x30F5,0x08);//�迹ƥ��
	
	//
	S5K4E1GA_write_cmos_sensor(0x30C1,0x01);//pack video enable [0]
	S5K4E1GA_write_cmos_sensor(0x30EE,0x02);//DPHY enable [ 1]
    S5K4E1GA_write_cmos_sensor(0x3111,0x86);//Embedded data off [5]

	//S5K4E1GA_write_cmos_sensor(0x0202, 0x03); //
	//S5K4E1GA_write_cmos_sensor(0x0203, 0x14); //A8
	//4X
	//S5K4E1GA_write_cmos_sensor(0x0204, 0x00); //analog gain[msb] 0100 x8 0080 x4
	//S5K4E1GA_write_cmos_sensor(0x0205, 0x80); //analog gain[lsb] 0040 x2 0020 x1
	
	//if(s5k4e1ga.maxExposureLines > 0x3e0 )
	//{
	//}else{
	S5K4E1GA_write_cmos_sensor(0x0340, 0x03);
	S5K4E1GA_write_cmos_sensor(0x0341, 0xE0);
	//}	
	// Line Length: 2738
	//S5K4E1GA_write_cmos_sensor(0x0342, (S5K4E1GA_PV_PERIOD_PIXEL_NUMS>>8)&0xff);
	//S5K4E1GA_write_cmos_sensor(0x0342, S5K4E1GA_PV_PERIOD_PIXEL_NUMS&0xff);
	S5K4E1GA_write_cmos_sensor(0x0342, (S5K4E1GA_PV_PERIOD_PIXEL_NUMS>>8)&0xFF);
	S5K4E1GA_write_cmos_sensor(0x0343, (S5K4E1GA_PV_PERIOD_PIXEL_NUMS&0xFF));
	
	#if defined(MIPI_2_LANE)
        S5K4E1GA_write_cmos_sensor(0x0305, 0x04); //P=4
		S5K4E1GA_write_cmos_sensor(0x0306, 0x00); 
		S5K4E1GA_write_cmos_sensor(0x0307, 0x49); //M=50 ;0X36--648;0X44->816
		//reserved
		S5K4E1GA_write_cmos_sensor(0x30B5, 0x01);//0x00
		S5K4E1GA_write_cmos_sensor(0x30E2, 0x02); //outif_number_of_lanes
		S5K4E1GA_write_cmos_sensor(0x30F1, 0x70); //0XB0
	#else
		S5K4E1GA_write_cmos_sensor(0x0305, 0x04); //P=4
		S5K4E1GA_write_cmos_sensor(0x0306, 0x00); 
		S5K4E1GA_write_cmos_sensor(0x0307, 0x49); //M=50 ;0X36--648;0X44->816
		//reserved
		S5K4E1GA_write_cmos_sensor(0x30B5, 0x00);
		S5K4E1GA_write_cmos_sensor(0x30E2, 0x01); //outif_number_of_lanes
		S5K4E1GA_write_cmos_sensor(0x30F1, 0xD0); //0XB0
	#endif
	
	//24M/4*2*50/1/10=60M
	//24M/4*2*54/1/10=64.8M
	//24M/4*2*68/1/10=81.6M
	// 1 lane 10bit /10; 2 lane 10bit /5
	// 1 lane 8bit /8; 2 lane 8bit /4
	//648
	//0xA : 560Mbps  ~ 640 Mbps
	//0xB : 640 Mbps ~ 690 Mbps
	//0xD : 770 Mbps ~ 860 Mbps
	
	// Size Setting ...
	// 1304 x 980

	S5K4E1GA_write_cmos_sensor(0x30A9,0x02);//Horizontal Binning On 	
  	S5K4E1GA_write_cmos_sensor(0x300E,0xEB);//Vertical Binning On
	
	S5K4E1GA_write_cmos_sensor(0x0344,0x00);//x_addr_start 0
	S5K4E1GA_write_cmos_sensor(0x0345,0x00);
	S5K4E1GA_write_cmos_sensor(0x0348,0x0A);//x_addr_end 2607
   	S5K4E1GA_write_cmos_sensor(0x0349,0x2F);
	S5K4E1GA_write_cmos_sensor(0x0346,0x00);//y_addr_start 0
	S5K4E1GA_write_cmos_sensor(0x0347,0x00);
	S5K4E1GA_write_cmos_sensor(0x034A,0x07);//y_addr_end 1959
    S5K4E1GA_write_cmos_sensor(0x034B,0xA7);

	//////////////for 1304*980 //////////////
	S5K4E1GA_write_cmos_sensor(0x0380,0x00);//x_even_inc 1
	S5K4E1GA_write_cmos_sensor(0x0381,0x01);
	S5K4E1GA_write_cmos_sensor(0x0382,0x00);//x_odd_inc 1
    S5K4E1GA_write_cmos_sensor(0x0383,0x01);
	S5K4E1GA_write_cmos_sensor(0x0384,0x00);//y_even_inc 1
	S5K4E1GA_write_cmos_sensor(0x0385,0x01);
	S5K4E1GA_write_cmos_sensor(0x0386,0x00);//y_odd_inc 3
    S5K4E1GA_write_cmos_sensor(0x0387,0x03);
	
	S5K4E1GA_write_cmos_sensor(0x034C,0x05);//x_output_size 1304
	S5K4E1GA_write_cmos_sensor(0x034D,0x18);
	S5K4E1GA_write_cmos_sensor(0x034E,0x03);//y_output_size 980
	S5K4E1GA_write_cmos_sensor(0x034F,0xd4);

	//S5K4E1GA_write_cmos_sensor(0x30BE,0x1A);//M_PCLKDIV_AUTO[4], M_DIV_PCLK[3:0]
    //S5K4E1GA_write_cmos_sensor(0x30BF,0xAB);//outif_enable[7], data_type[5:0](2Bh = bayer 10bit)
    
	S5K4E1GA_write_cmos_sensor(0x30C0,0xA0);//video_offset[7:4] 3260=(2608*1.25)%12
	S5K4E1GA_write_cmos_sensor(0x30C8,0x06);//video_data_length 1630 = 1304 * 1.25
	S5K4E1GA_write_cmos_sensor(0x30C9,0x5E);

	//enable line start, line end
	u16Temp=S5K4E1GA_read_cmos_sensor(0x30C7);
	SENSORDB("u16Temp=0x%x",u16Temp);
	u16Temp|=0xc;
	S5K4E1GA_write_cmos_sensor(0x30c7,u16Temp&0xff);
	SENSORDB("0x30C7=0x%x",S5K4E1GA_read_cmos_sensor(0x30C7));
		//S5K4E1GASetTestPatternMode(1);
	S5K4E1GA_write_cmos_sensor(0x0105,0x01);
	// Operating START
    S5K4E1GA_write_cmos_sensor(0x0100,0x01);	
	mdelay(30);
}   /*  S5K4E1GA_Sensor_Init  */

void S5K4E1GACaptureSetting(void)
{	
	kal_uint16 u16Temp=0;
    SENSORDB("enter!");
	//#if 0
	S5K4E1GA_write_cmos_sensor(0x0100, 0x00);  // SW stand-by	
	
	S5K4E1GA_write_cmos_sensor(0x0103, 0x01);  //software reset
    S5K4E1GA_write_cmos_sensor(0x3030, 0x06);  //RAW 10

	S5K4E1GA_write_cmos_sensor(0x3000,0x05);
	S5K4E1GA_write_cmos_sensor(0x3001,0x03);
    S5K4E1GA_write_cmos_sensor(0x3002,0x08);
	S5K4E1GA_write_cmos_sensor(0x3003,0x09);
	S5K4E1GA_write_cmos_sensor(0x3004,0x2E);
	S5K4E1GA_write_cmos_sensor(0x3005,0x06);
    S5K4E1GA_write_cmos_sensor(0x3006,0x34);
	S5K4E1GA_write_cmos_sensor(0x3007,0x00);
	S5K4E1GA_write_cmos_sensor(0x3008,0x3C);
	S5K4E1GA_write_cmos_sensor(0x3009,0x3C);
    S5K4E1GA_write_cmos_sensor(0x300A,0x28);
	S5K4E1GA_write_cmos_sensor(0x300B,0x04);
	S5K4E1GA_write_cmos_sensor(0x300C,0x0A);
	S5K4E1GA_write_cmos_sensor(0x300D,0x02);
	S5K4E1GA_write_cmos_sensor(0x300E,0xE8);
    S5K4E1GA_write_cmos_sensor(0x300F,0x82);
	S5K4E1GA_write_cmos_sensor(0x3010,0x00);
	S5K4E1GA_write_cmos_sensor(0x3011,0x4C);
	S5K4E1GA_write_cmos_sensor(0x3012,0x30);
    S5K4E1GA_write_cmos_sensor(0x3013,0xC0);
	S5K4E1GA_write_cmos_sensor(0x3014,0x00);
	S5K4E1GA_write_cmos_sensor(0x3015,0x00);
	S5K4E1GA_write_cmos_sensor(0x3016,0x2C);
    S5K4E1GA_write_cmos_sensor(0x3017,0x94);
	S5K4E1GA_write_cmos_sensor(0x3018,0x78);

	S5K4E1GA_write_cmos_sensor(0x301B,0x75);

	S5K4E1GA_write_cmos_sensor(0x301C,0x04);
	S5K4E1GA_write_cmos_sensor(0x301D,0xD4);
	//0x301e?
	
    S5K4E1GA_write_cmos_sensor(0x3021,0x02);
	S5K4E1GA_write_cmos_sensor(0x3022,0x24);
	S5K4E1GA_write_cmos_sensor(0x3024,0x40);
	S5K4E1GA_write_cmos_sensor(0x3027,0x08);
	
    S5K4E1GA_write_cmos_sensor(0x3029,0xC6); //0x06  ERIC
    S5K4E1GA_write_cmos_sensor(0x30BC,0xA8);// 0XB0/////////////////////////////
	S5K4E1GA_write_cmos_sensor(0x302B,0x01); //0x00  ERIC
	S5K4E1GA_write_cmos_sensor(0x30D8,0x3F);

	// ADLC setting ...
    S5K4E1GA_write_cmos_sensor(0x3070,0x5F);
	S5K4E1GA_write_cmos_sensor(0x3071,0x00);
	S5K4E1GA_write_cmos_sensor(0x3080,0x04);
	S5K4E1GA_write_cmos_sensor(0x3081,0x38);
	// MIPI setting
	S5K4E1GA_write_cmos_sensor(0x30BD,0x00);//SEL_CCP[0]
    S5K4E1GA_write_cmos_sensor(0x3084,0x15);//SYNC Mode
	S5K4E1GA_write_cmos_sensor(0x30BE,0x1A);//M_PCLKDIV_AUTO[4], M_DIV_PCLK[3:0]
	S5K4E1GA_write_cmos_sensor(0x30BF,0xAB);
	//
	S5K4E1GA_write_cmos_sensor(0x30C1,0x01);//pack video enable [0]
	S5K4E1GA_write_cmos_sensor(0x30EE,0x02);//DPHY enable [ 1]
    S5K4E1GA_write_cmos_sensor(0x3111,0x86);//Embedded data off [5]
	
	/*// Integration setting
	// 1576
	S5K4E1GA_write_cmos_sensor(0x0202, 0x07); //07//coarse integration time
	S5K4E1GA_write_cmos_sensor(0x0203, 0xa8); //A8
	// 4x
	S5K4E1GA_write_cmos_sensor(0x0204, 0x00); //analog gain[msb] 0100 x8 0080 x4
	S5K4E1GA_write_cmos_sensor(0x0205, 0x80); //analog gain[lsb] 0040 x2 0020 x1
	*/
	// Frame Length
	//if( s5k4e1ga.maxExposureLines > 0x7B4 )
	//{
	
	//}
	//else
	//{
	S5K4E1GA_write_cmos_sensor(0x0340, 0x07);
	S5K4E1GA_write_cmos_sensor(0x0341, 0xB4);
	//}
	
	// Line Length
	S5K4E1GA_write_cmos_sensor(0x0342, (S5K4E1GA_FULL_PERIOD_PIXEL_NUMS>>8)&0xFF);
	S5K4E1GA_write_cmos_sensor(0x0343, (S5K4E1GA_FULL_PERIOD_PIXEL_NUMS&0xFF));

	#if defined(MIPI_2_LANE)
	S5K4E1GA_write_cmos_sensor(0x0305, 0x04);
	S5K4E1GA_write_cmos_sensor(0x0306, 0x00);
	S5K4E1GA_write_cmos_sensor(0x0307, 0x49);
	//reserved
	S5K4E1GA_write_cmos_sensor(0x30B5, 0x01);
	S5K4E1GA_write_cmos_sensor(0x30E2, 0x02);  //outif_number_of_lanes
	S5K4E1GA_write_cmos_sensor(0x30F1, 0x70); //0xA0
	#else
	// PLL setting ...
	//// input clock 24MHz
	S5K4E1GA_write_cmos_sensor(0x0305, 0x04);
	S5K4E1GA_write_cmos_sensor(0x0306, 0x00);
	S5K4E1GA_write_cmos_sensor(0x0307, 0x49);
	//reserved
	S5K4E1GA_write_cmos_sensor(0x30B5, 0x00);
	S5K4E1GA_write_cmos_sensor(0x30E2, 0x01);  //outif_number_of_lanes
	S5K4E1GA_write_cmos_sensor(0x30F1, 0xD0);
	#endif
	// Size Setting

	S5K4E1GA_write_cmos_sensor(0x30A9, 0x03); //Horizontal Binning Off

	SENSORDB("0x30a9=0x%x",S5K4E1GA_read_cmos_sensor(0x30a9));
	S5K4E1GA_write_cmos_sensor(0x300E, 0xE8); //Vertical Binning Off
	//////////////for 1304*980 //////////////
	
	S5K4E1GA_write_cmos_sensor(0x0344,0x00);//x_addr_start 0
	S5K4E1GA_write_cmos_sensor(0x0345,0x00);
	S5K4E1GA_write_cmos_sensor(0x0348,0x0A);//x_addr_end 2607
   	S5K4E1GA_write_cmos_sensor(0x0349,0x2F);
	S5K4E1GA_write_cmos_sensor(0x0346,0x00);//y_addr_start 0
	S5K4E1GA_write_cmos_sensor(0x0347,0x00);
	S5K4E1GA_write_cmos_sensor(0x034A,0x07);//y_addr_end 1959
    S5K4E1GA_write_cmos_sensor(0x034B,0xA7);
	
	S5K4E1GA_write_cmos_sensor(0x0380,0x00);//x_even_inc 1
	S5K4E1GA_write_cmos_sensor(0x0381,0x01);
	S5K4E1GA_write_cmos_sensor(0x0382,0x00);//x_odd_inc 1
   	S5K4E1GA_write_cmos_sensor(0x0383,0x01);
	S5K4E1GA_write_cmos_sensor(0x0384,0x00);//y_even_inc 1
	S5K4E1GA_write_cmos_sensor(0x0385,0x01);
	S5K4E1GA_write_cmos_sensor(0x0386,0x00);//y_odd_inc 
    S5K4E1GA_write_cmos_sensor(0x0387,0x01);
	
	S5K4E1GA_write_cmos_sensor(0x034C,0x0A);//x_output_size 2608
	S5K4E1GA_write_cmos_sensor(0x034D,0x30);
	S5K4E1GA_write_cmos_sensor(0x034E,0x07);//y_output_size 1960
	S5K4E1GA_write_cmos_sensor(0x034F,0xA8);

	
	//S5K4E1GA_write_cmos_sensor(0x30BE,0x1A);//M_PCLKDIV_AUTO[4], M_DIV_PCLK[3:0]
    //S5K4E1GA_write_cmos_sensor(0x30BF,0xAB);//outif_enable[7], data_type[5:0](2Bh = bayer 10bit)
    
	S5K4E1GA_write_cmos_sensor(0x30C0,0x80);//video_offset[7:4] 3260%12
	S5K4E1GA_write_cmos_sensor(0x30C8,0x0C);//video_data_length 1600 = 1304 * 1.25
	S5K4E1GA_write_cmos_sensor(0x30C9,0xBC);

	//enable line start, line end
	u16Temp=S5K4E1GA_read_cmos_sensor(0x30C7);
	SENSORDB("u16Temp=0x%x",u16Temp);
	u16Temp|=0xc;
	S5K4E1GA_write_cmos_sensor(0x30c7,u16Temp&0xff);
	SENSORDB("0x30C7=0x%x",S5K4E1GA_read_cmos_sensor(0x30C7));
	
	// Operating START
    S5K4E1GA_write_cmos_sensor(0x0100,0x01);
	mdelay(20);
}

static void S5K4E1GA_Sensor_Init(void)
{
	SENSORDB("enter!");
	    
	#if defined( S5K4E1GA_1M_SIZE_PREVIEW )
		S5K4E1GAPreviewSetting(); // make sure after open sensor, there are normal signal output
	#else
		S5K4E1GACaptureSetting(); 
	#endif	
} 

/*************************************************************************
* FUNCTION
*   S5K4E1GAOpen
*
* DESCRIPTION
*   This function initialize the registers of CMOS sensor
*
* PARAMETERS
*   None
*
* RETURNS
*   None
*
* GLOBALS AFFECTED
*
*************************************************************************/

UINT32 S5K4E1GAOpen(void)
{

	volatile signed int i;
	kal_uint16 sensor_id = 0;
	
	S5K4E1GA_write_cmos_sensor(0x0103,0x01);// Reset sensor
    mdelay(10);

	//  Read sensor ID to adjust I2C is OK?
	for(i=0;i<3;i++)
	{
		sensor_id = (S5K4E1GA_read_cmos_sensor(0x0000)<<8)|S5K4E1GA_read_cmos_sensor(0x0001);
		SENSORDB("sensor_id=0x%x",sensor_id);
		if(S5K4E1GA_SENSOR_ID==sensor_id)
		{
			break;
		}
	}
	if(sensor_id != S5K4E1GA_SENSOR_ID)
	{
		return ERROR_SENSOR_CONNECT_FAIL;
	}

	S5K4E1GA_Sensor_Init();
	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s5k4e1ga.sensorMode = SENSOR_MODE_INIT;

	s5k4e1ga.DummyLines= 0;
	s5k4e1ga.DummyPixels= 0;
	
	s5k4e1ga.pvPclk = 876;
	s5k4e1ga.capPclk = 876;
	
	s5k4e1ga.shutter = 0x314;
	s5k4e1ga.maxExposureLines =S5K4E1GA_PV_PERIOD_LINE_NUMS;

	s5k4e1ga.sensorBaseGain = 0x20;
	s5k4e1ga.ispBaseGain = BASEGAIN;
	s5k4e1ga.sensorGlobalGain = (4 * s5k4e1ga.sensorBaseGain);
	spin_unlock(&s5k4e1gamipiraw_drv_lock);	
    return ERROR_NONE;
}

/*************************************************************************
* FUNCTION
*   S5K4E1GAGetSensorID
*
* DESCRIPTION
*   This function get the sensor ID 
*
* PARAMETERS
*   *sensorID : return the sensor ID 
*
* RETURNS
*   None
*
* GLOBALS AFFECTED
*
*************************************************************************/
UINT32 S5K4E1GAGetSensorID(UINT32 *sensorID) 
{
    int  retry = 3; 
	S5K4E1GA_write_cmos_sensor(0x0103,0x01);// Reset sensor
    mdelay(10);
	
    // check if sensor ID correct
    do {
        *sensorID = (S5K4E1GA_read_cmos_sensor(0x0000)<<8)|S5K4E1GA_read_cmos_sensor(0x0001);  
		SENSORDB("Sensor ID = 0x%04x", *sensorID);
        if (*sensorID == S5K4E1GA_SENSOR_ID)
    	{
        	break; 
    	}
        retry--; 
    } while (retry > 0);

    if (S5K4E1GA_SENSOR_ID != *sensorID) 
	{
        *sensorID = 0xFFFFFFFF; 
        return ERROR_SENSOR_CONNECT_FAIL;
    }
    return ERROR_NONE;
}


/*************************************************************************
* FUNCTION
*   S5K4E1GA_SetShutter
*
* DESCRIPTION
*   This function set e-shutter of S5K4E1GA to change exposure time.
*
* PARAMETERS
*   shutter : exposured lines
*
* RETURNS
*   None
*
* GLOBALS AFFECTED
*
*************************************************************************/
void S5K4E1GA_SetShutter(kal_uint16 iShutter)
{
	unsigned long flags;
	
	spin_lock_irqsave(&s5k4e1gamipiraw_drv_lock,flags);
	s5k4e1ga.shutter= iShutter;
	spin_unlock_irqrestore(&s5k4e1gamipiraw_drv_lock,flags);
	
	S5K4E1GA_write_shutter(iShutter);
	return;
}   /*  S5K4E1GA_SetShutter   */



/*************************************************************************
* FUNCTION
*   S5K4E1GA_read_shutter
*
* DESCRIPTION
*   This function to  Get exposure time.
*
* PARAMETERS
*   None
*
* RETURNS
*   shutter : exposured lines
*
* GLOBALS AFFECTED
*
*************************************************************************/
UINT16 S5K4E1GA_read_shutter(void)
{
    UINT16 shutter = 0;

	shutter = (S5K4E1GA_read_cmos_sensor(0x0202) << 8) | S5K4E1GA_read_cmos_sensor(0x0203);

	return shutter;
}

/*************************************************************************
* FUNCTION
*   S5K4E1GA_night_mode
*
* DESCRIPTION
*   This function night mode of S5K4E1GA.
*
* PARAMETERS
*   none
*
* RETURNS
*   None
*
* GLOBALS AFFECTED
*
*************************************************************************/
void S5K4E1GA_NightMode(kal_bool bEnable)
{
}/*	S5K4E1GA_NightMode */



/*************************************************************************
* FUNCTION
*   S5K4E1GAClose
*
* DESCRIPTION
*   This function is to turn off sensor module power.
*
* PARAMETERS
*   None
*
* RETURNS
*   None
*
* GLOBALS AFFECTED
*
*************************************************************************/
UINT32 S5K4E1GAClose(void)
{
    //  CISModulePowerOn(FALSE);
    //s_porting
    //  DRV_I2CClose(S5K4E1GAhDrvI2C);
    //e_porting
    return ERROR_NONE;
}	/* S5K4E1GAClose() */

void S5K4E1GASetFlipMirror(kal_int32 imgMirror)
{
	SENSORDB("imgMirror=%d\n",imgMirror);
    switch (imgMirror)
    {
        case IMAGE_NORMAL: //B
            S5K4E1GA_write_cmos_sensor(0x0101, 0x03);	//Set normal
            break;
        case IMAGE_V_MIRROR: //Gr X
            S5K4E1GA_write_cmos_sensor(0x0101, 0x01);	//Set flip
            break;
        case IMAGE_H_MIRROR: //Gb
            S5K4E1GA_write_cmos_sensor(0x0101, 0x02);	//Set mirror
            break;
        case IMAGE_HV_MIRROR: //R
            S5K4E1GA_write_cmos_sensor(0x0101, 0x00);	//Set mirror and flip
            break;
    }
	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s5k4e1ga.imgMirror = imgMirror;
	spin_unlock(&s5k4e1gamipiraw_drv_lock);
}


UINT32 S5K4E1GASetTestPatternMode(kal_bool bEnable)
{
	kal_uint32 reg30a9 = 0x00;
    SENSORDB("[S5K4E1GASetTestPatternMode] Test pattern enable:%d\n", bEnable);

	reg30a9 = S5K4E1GA_read_cmos_sensor(0x30a9);
    if(bEnable) 
    { 
      // enable color bar   
      spin_lock(&s5k4e1gamipiraw_drv_lock);
      test_pattern = 1;
	  spin_unlock(&s5k4e1gamipiraw_drv_lock);
	  reg30a9 = reg30a9 | 0x08;
      S5K4E1GA_write_cmos_sensor(0x30a9, reg30a9);  // color bar test pattern
      S5K4E1GA_write_cmos_sensor(0x0601, 0x02);  // color bar test pattern 
    } 
    else 
    {
       spin_lock(&s5k4e1gamipiraw_drv_lock);
       test_pattern = 0;
	   spin_unlock(&s5k4e1gamipiraw_drv_lock);
	   reg30a9 = reg30a9 & 0x07;
       S5K4E1GA_write_cmos_sensor(0x30a9, reg30a9);  // disable color bar test pattern
       S5K4E1GA_write_cmos_sensor(0x0601, 0x00); 
    }

	SENSORDB("[S5K4E1GASetTestPatternMode] Test pattern enable:%d, reg30a9=0x%x\n", bEnable, reg30a9);
    return TRUE;
}



/*************************************************************************
* FUNCTION
*   S5K4E1GAPreview
*
* DESCRIPTION
*   This function start the sensor preview.
*
* PARAMETERS
*   *image_window : address pointer of pixel numbers in one period of HSYNC
*  *sensor_config_data : address pointer of line numbers in one period of VSYNC
*
* RETURNS
*   None
*
* GLOBALS AFFECTED
*
*************************************************************************/
UINT32 S5K4E1GAPreview(MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
                                                MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{

	SENSORDB("enter!");
	
	S5K4E1GAPreviewSetting();
	
	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s5k4e1ga.sensorMode = SENSOR_MODE_PREVIEW; // Need set preview setting after capture mode
	spin_unlock(&s5k4e1gamipiraw_drv_lock);

	//set mirror & flip
	S5K4E1GASetFlipMirror(sensor_config_data->SensorImageMirror);
	
	//set dummy
	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s5k4e1ga.DummyPixels=0;
	s5k4e1ga.DummyLines=0;	
	spin_unlock(&s5k4e1gamipiraw_drv_lock);
	S5K4E1GA_SetDummy( s5k4e1ga.DummyPixels, s5k4e1ga.DummyLines);
	S5K4E1GASetTestPatternMode(test_pattern);
	memcpy(&S5K4E1GASensorConfigData, sensor_config_data, sizeof(MSDK_SENSOR_CONFIG_STRUCT));
	
    return ERROR_NONE;
}	/* S5K4E1GAPreview() */

UINT32 S5K4E1GAVideoPreview(MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
                                                MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	SENSORDB("enter!");
	S5K4E1GAPreview(image_window,sensor_config_data);
}

static UINT32 S5K4E1GAZSDPreview(MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
                                                MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
 	kal_uint16 shutter = s5k4e1ga.shutter;
	kal_uint16 pv_line_length , cap_line_length,cap_frame_length,temp_data;
	
	SENSORDB("enter!");
	
	
	S5K4E1GACaptureSetting();
		
	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s5k4e1ga.sensorMode = SENSOR_MODE_ZSD_PREVIEW;	
	spin_unlock(&s5k4e1gamipiraw_drv_lock);
	
	//set mirror & flip
	S5K4E1GASetFlipMirror(sensor_config_data->SensorImageMirror);
	
	// set dummy
	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s5k4e1ga.DummyPixels =0;
	s5k4e1ga.DummyLines  =0;
	spin_unlock(&s5k4e1gamipiraw_drv_lock);
	S5K4E1GA_SetDummy( s5k4e1ga.DummyPixels, s5k4e1ga.DummyLines);
	
	
	S5K4E1GA_write_shutter(shutter);	
	
    memcpy(&S5K4E1GASensorConfigData, sensor_config_data, sizeof(MSDK_SENSOR_CONFIG_STRUCT));
	
    return ERROR_NONE;
}


static UINT32 S5K4E1GACapture(MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
                                                MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	SENSORDB("enter!");
	S5K4E1GACaptureSetting();
	S5K4E1GASetTestPatternMode(test_pattern);
	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s5k4e1ga.sensorMode = SENSOR_MODE_CAPTURE;	
	spin_unlock(&s5k4e1gamipiraw_drv_lock);	
	//set mirror & flip
	S5K4E1GASetFlipMirror(sensor_config_data->SensorImageMirror);  
    return ERROR_NONE;
}	/* S5K4E1GACapture() */

UINT32 S5K4E1GAGetResolution(MSDK_SENSOR_RESOLUTION_INFO_STRUCT *pSensorResolution)
{

    SENSORDB("enter!");
	#if defined( S5K4E1GA_1M_SIZE_PREVIEW )
		pSensorResolution->SensorPreviewWidth	= S5K4E1GA_IMAGE_SENSOR_PV_WIDTH;
    	pSensorResolution->SensorPreviewHeight	= S5K4E1GA_IMAGE_SENSOR_PV_HEIGHT;
	#else
    	pSensorResolution->SensorPreviewWidth	= S5K4E1GA_IMAGE_SENSOR_FULL_WIDTH; //S5K4E1GAMIPI_REAL_PV_WIDTH;
    	pSensorResolution->SensorPreviewHeight	= S5K4E1GA_IMAGE_SENSOR_FULL_HEIGHT; //S5K4E1GAMIPI_REAL_PV_HEIGHT;
    #endif
    pSensorResolution->SensorFullWidth		= S5K4E1GA_IMAGE_SENSOR_FULL_WIDTH; //S5K4E1GAMIPI_REAL_CAP_WIDTH;
    pSensorResolution->SensorFullHeight		= S5K4E1GA_IMAGE_SENSOR_FULL_HEIGHT; //S5K4E1GAMIPI_REAL_CAP_HEIGHT;
    
    pSensorResolution->SensorVideoWidth		= S5K4E1GA_IMAGE_SENSOR_VIDEO_WIDTH;
    pSensorResolution->SensorVideoHeight    = S5K4E1GA_IMAGE_SENSOR_VIDEO_HEIGHT;
    return ERROR_NONE;
}   /* S5K4E1GAGetResolution() */

UINT32 S5K4E1GAGetInfo(MSDK_SCENARIO_ID_ENUM ScenarioId,
                                                MSDK_SENSOR_INFO_STRUCT *pSensorInfo,
                                                MSDK_SENSOR_CONFIG_STRUCT *pSensorConfigData)
{
	#if defined( S5K4E1GA_1M_SIZE_PREVIEW )
			switch(ScenarioId){
				case MSDK_SCENARIO_ID_CAMERA_ZSD:
					pSensorInfo->SensorPreviewResolutionX= S5K4E1GA_IMAGE_SENSOR_FULL_WIDTH; //S5K4E1GA_IMAGE_SENSOR_FULL_WIDTH;
					pSensorInfo->SensorPreviewResolutionY=S5K4E1GA_IMAGE_SENSOR_FULL_HEIGHT ;//S5K4E1GAMIPI_REAL_CAP_HEIGHT;
					pSensorInfo->SensorCameraPreviewFrameRate=15;
					break;

				default:
					pSensorInfo->SensorPreviewResolutionX= S5K4E1GA_IMAGE_SENSOR_PV_WIDTH; //S5K4E1GAMIPI_REAL_PV_WIDTH;
        			pSensorInfo->SensorPreviewResolutionY= S5K4E1GA_IMAGE_SENSOR_PV_HEIGHT; //S5K4E1GAMIPI_REAL_PV_HEIGHT;
					pSensorInfo->SensorCameraPreviewFrameRate=30;
					break;
			}		
	#else
		pSensorInfo->SensorPreviewResolutionX= S5K4E1GA_IMAGE_SENSOR_FULL_WIDTH; //S5K4E1GA_IMAGE_SENSOR_FULL_WIDTH;
		pSensorInfo->SensorPreviewResolutionY=S5K4E1GA_IMAGE_SENSOR_FULL_HEIGHT ;//S5K4E1GAMIPI_REAL_CAP_HEIGHT;
		pSensorInfo->SensorCameraPreviewFrameRate=15;
	#endif
	pSensorInfo->SensorFullResolutionX= S5K4E1GA_IMAGE_SENSOR_FULL_WIDTH;
    pSensorInfo->SensorFullResolutionY= S5K4E1GA_IMAGE_SENSOR_FULL_HEIGHT;

	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s5k4e1ga.imgMirror = pSensorConfigData->SensorImageMirror ;
	spin_unlock(&s5k4e1gamipiraw_drv_lock);
	SENSORDB("imgMirror=%d\n", s5k4e1ga.imgMirror );

	switch(s5k4e1ga.imgMirror)
	{
		case IMAGE_NORMAL: 
			 pSensorInfo->SensorOutputDataFormat = SENSOR_OUTPUT_FORMAT_RAW_B; //SENSOR_OUTPUT_FORMAT_RAW_Gb ;
			 break;
		case IMAGE_H_MIRROR: 
			 pSensorInfo->SensorOutputDataFormat = SENSOR_OUTPUT_FORMAT_RAW_Gb;
			 break;
	    case IMAGE_V_MIRROR: 
			 pSensorInfo->SensorOutputDataFormat = SENSOR_OUTPUT_FORMAT_RAW_Gr;
			 break;
	    case IMAGE_HV_MIRROR: 
			 pSensorInfo->SensorOutputDataFormat = SENSOR_OUTPUT_FORMAT_RAW_R ;
			 break;
		default:
			break;
	}
	
    //pSensorInfo->SensorVideoFrameRate=30;
    //pSensorInfo->SensorStillCaptureFrameRate=15;
    //pSensorInfo->SensorWebCamCaptureFrameRate=15;
    //pSensorInfo->SensorResetActiveHigh=FALSE;
    //pSensorInfo->SensorResetDelayCount=5;
   	//pSensorInfo->SensorOutputDataFormat=SENSOR_OUTPUT_FORMAT_RAW_B;
    pSensorInfo->SensorClockPolarity=SENSOR_CLOCK_POLARITY_LOW; /*??? */
    pSensorInfo->SensorClockFallingPolarity=SENSOR_CLOCK_POLARITY_LOW;
    pSensorInfo->SensorHsyncPolarity = SENSOR_CLOCK_POLARITY_LOW;
    pSensorInfo->SensorVsyncPolarity = SENSOR_CLOCK_POLARITY_LOW;
    //pSensorInfo->SensorInterruptDelayLines = 1;
    pSensorInfo->SensroInterfaceType=SENSOR_INTERFACE_TYPE_MIPI;
   
    pSensorInfo->CaptureDelayFrame = 2; 
    pSensorInfo->PreviewDelayFrame = 1; 
    pSensorInfo->VideoDelayFrame = 5; 
    //pSensorInfo->SensorMasterClockSwitch = 0; 
    pSensorInfo->SensorDrivingCurrent = ISP_DRIVING_8MA;      
    pSensorInfo->AEShutDelayFrame = 0;//0;		    /* The frame of setting shutter default 0 for TG int */
    pSensorInfo->AESensorGainDelayFrame = 0 ;//0;     /* The frame of setting sensor gain */
    pSensorInfo->AEISPGainDelayFrame = 2;	
	   
    switch (ScenarioId)
    {
        case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
        case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
            pSensorInfo->SensorClockFreq=24;
            pSensorInfo->SensorClockDividCount=	5;
            pSensorInfo->SensorClockRisingCount= 0;
            pSensorInfo->SensorClockFallingCount= 2;
            pSensorInfo->SensorPixelClockCount= 3;
            pSensorInfo->SensorDataLatchCount= 2;
            pSensorInfo->SensorGrabStartX = S5K4E1GA_PV_X_START; 
            pSensorInfo->SensorGrabStartY = S5K4E1GA_PV_Y_START;  
            pSensorInfo->SensorMIPILaneNumber = SENSOR_MIPI_2_LANE;	
            pSensorInfo->MIPIDataLowPwr2HighSpeedTermDelayCount = 0; 
	     	pSensorInfo->MIPIDataLowPwr2HighSpeedSettleDelayCount = 14; 
	    	pSensorInfo->MIPICLKLowPwr2HighSpeedTermDelayCount = 0;
            pSensorInfo->SensorWidthSampling = 0;  // 0 is default 1x
            pSensorInfo->SensorHightSampling = 0;   // 0 is default 1x 
            pSensorInfo->SensorPacketECCOrder = 1;
            break;
        case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
		case MSDK_SCENARIO_ID_CAMERA_ZSD:
            pSensorInfo->SensorClockFreq=24;
            pSensorInfo->SensorClockDividCount=	5;
            pSensorInfo->SensorClockRisingCount= 0;
            pSensorInfo->SensorClockFallingCount= 2;
            pSensorInfo->SensorPixelClockCount= 3;
            pSensorInfo->SensorDataLatchCount= 2;
            pSensorInfo->SensorGrabStartX = S5K4E1GA_FULL_X_START;	//2*S5K4E1GA_IMAGE_SENSOR_PV_STARTX; 
            pSensorInfo->SensorGrabStartY = S5K4E1GA_FULL_Y_START;	//2*S5K4E1GA_IMAGE_SENSOR_PV_STARTY;          			
            pSensorInfo->SensorMIPILaneNumber = SENSOR_MIPI_2_LANE;		
            pSensorInfo->MIPIDataLowPwr2HighSpeedTermDelayCount = 0; 
            pSensorInfo->MIPIDataLowPwr2HighSpeedSettleDelayCount = 14; 
            pSensorInfo->MIPICLKLowPwr2HighSpeedTermDelayCount = 0; 
            pSensorInfo->SensorWidthSampling = 0;  // 0 is default 1x
            pSensorInfo->SensorHightSampling = 0;   // 0 is default 1x
            pSensorInfo->SensorPacketECCOrder = 1;
            break;
        default:
            pSensorInfo->SensorClockFreq=24;
            pSensorInfo->SensorClockDividCount=	3;
            pSensorInfo->SensorClockRisingCount= 0;
            pSensorInfo->SensorClockFallingCount= 2;
            pSensorInfo->SensorPixelClockCount= 3;
            pSensorInfo->SensorDataLatchCount= 2;
            pSensorInfo->SensorGrabStartX = 1; 
            pSensorInfo->SensorGrabStartY = 1;  
            pSensorInfo->SensorMIPILaneNumber = SENSOR_MIPI_2_LANE;		
            pSensorInfo->MIPIDataLowPwr2HighSpeedTermDelayCount = 0; 
            pSensorInfo->MIPIDataLowPwr2HighSpeedSettleDelayCount = 14; 
            pSensorInfo->MIPICLKLowPwr2HighSpeedTermDelayCount = 0; 
            break;
    }

    memcpy(pSensorConfigData, &S5K4E1GASensorConfigData, sizeof(MSDK_SENSOR_CONFIG_STRUCT));

    return ERROR_NONE;
}   /* S5K4E1GAGetInfo() */


UINT32 S5K4E1GAControl(MSDK_SCENARIO_ID_ENUM ScenarioId, MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *pImageWindow,
                                                MSDK_SENSOR_CONFIG_STRUCT *pSensorConfigData)
{
	SENSORDB("ScenarioId=%d\n",ScenarioId);
	spin_lock(&s5k4e1gamipiraw_drv_lock);
	s_CurrentScenarioId = ScenarioId;
	spin_unlock(&s5k4e1gamipiraw_drv_lock);
    switch (ScenarioId)
    {
        case MSDK_SCENARIO_ID_CAMERA_PREVIEW:     
        	S5K4E1GAPreview(pImageWindow, pSensorConfigData);
        break;
		case MSDK_SCENARIO_ID_VIDEO_PREVIEW:	
			S5K4E1GAVideoPreview(pImageWindow, pSensorConfigData);
		break;
        case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
			S5K4E1GACapture(pImageWindow, pSensorConfigData);
		break;
		case MSDK_SCENARIO_ID_CAMERA_ZSD:
			S5K4E1GAZSDPreview(pImageWindow, pSensorConfigData);       
        break;

        default:
            return ERROR_INVALID_SCENARIO_ID;

    }
    return ERROR_NONE;
} /* S5K4E1GAControl() */

UINT32 S5K4E1GetDefaultFramerateByScenario(MSDK_SCENARIO_ID_ENUM scenarioId, MUINT32 *pframeRate) 
{

	switch (scenarioId) 
	{
	case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
	case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
		 *pframeRate = 300;
	break;
	case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
	case MSDK_SCENARIO_ID_CAMERA_ZSD:
		 *pframeRate = 150;
	break;		
    case MSDK_SCENARIO_ID_CAMERA_3D_PREVIEW: //added
    case MSDK_SCENARIO_ID_CAMERA_3D_VIDEO:
    case MSDK_SCENARIO_ID_CAMERA_3D_CAPTURE: //added   
		 *pframeRate = 150;
	break;		
	default:
	break;
	}

	return ERROR_NONE;
}

static UINT32 S5K4E1SetMaxFramerateByScenario(MSDK_SCENARIO_ID_ENUM scenarioId, MUINT32 frameRate)
{
	kal_int16 dummyLine;
	kal_uint32 frameHeight;
		
	SENSORDB("scenarioId=%d,frameRate=%d\n",scenarioId,frameRate);
	switch (scenarioId) 
	{
	case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
	case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
		frameHeight = (10 * (kal_uint32)s5k4e1ga.pvPclk*100000)/frameRate/S5K4E1GA_PV_PERIOD_PIXEL_NUMS;
		dummyLine = (frameHeight>S5K4E1GA_PV_PERIOD_LINE_NUMS)?(frameHeight-S5K4E1GA_PV_PERIOD_LINE_NUMS):0;
		S5K4E1GA_SetDummy(0, dummyLine);			
	break;					
	case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
	case MSDK_SCENARIO_ID_CAMERA_ZSD:			
		frameHeight = (10 * (kal_uint32)s5k4e1ga.pvPclk*100000)/frameRate/S5K4E1GA_FULL_PERIOD_PIXEL_NUMS;
		dummyLine = (frameHeight>S5K4E1GA_PV_PERIOD_LINE_NUMS)?(frameHeight-S5K4E1GA_FULL_PERIOD_LINE_NUMS):0;
		S5K4E1GA_SetDummy(0, dummyLine);				
	break;		
    case MSDK_SCENARIO_ID_CAMERA_3D_PREVIEW: //added
    break;
    case MSDK_SCENARIO_ID_CAMERA_3D_VIDEO:
	break;
    case MSDK_SCENARIO_ID_CAMERA_3D_CAPTURE: //added   
	break;		
	default:
	break;
	}	
	return ERROR_NONE;
}

static UINT32 S5K4E1GASetFrameRate_Limit(UINT16 u2FrameRate)
{
	kal_int32 dummy_line;
	kal_int32 FrameHeight = s5k4e1ga.maxExposureLines;
	unsigned long flags;
	
	SENSORDB("u2FrameRate=%d\n",u2FrameRate);
	
	if(s5k4e1ga.sensorMode <= SENSOR_MODE_SMALL_SIZE_END)
	{
		FrameHeight= (s5k4e1ga.pvPclk*100000) / u2FrameRate * 10 / S5K4E1GA_PV_PERIOD_PIXEL_NUMS;
		dummy_line = FrameHeight - S5K4E1GA_PV_PERIOD_LINE_NUMS;

	}
	else if(s5k4e1ga.sensorMode <= SENSOR_MODE_FULL_SIZE_END)
	{
		FrameHeight= (s5k4e1ga.capPclk*100000) / u2FrameRate * 10 / S5K4E1GA_FULL_PERIOD_PIXEL_NUMS;
		dummy_line = FrameHeight - S5K4E1GA_FULL_PERIOD_LINE_NUMS;
	}
	spin_lock_irqsave(&s5k4e1gamipiraw_drv_lock,flags);
	s5k4e1ga.maxExposureLines = FrameHeight;
	spin_unlock_irqrestore(&s5k4e1gamipiraw_drv_lock,flags);
	
	SENSORDB("dummy_line=%d\n",dummy_line);
	dummy_line = (dummy_line>0?dummy_line:0);
	S5K4E1GA_SetDummy(0, dummy_line); /* modify dummy_pixel must gen AE table again */	
}

UINT32 S5K4E1GASetVideoMode(UINT16 u2FrameRate)
{
   /*
   		night mode:15fps
   		normal mode : 30fps @ 1M SIZE PREVIEW
     */
    kal_uint32 MAX_Frame_length =0,frameRate=0;
    SENSORDB("u2FrameRate=%d\n", u2FrameRate);
	if(u2FrameRate==0)
	{
		 SENSORDB("Do not fix framerate\n");
		return KAL_TRUE;
	}
	if(u2FrameRate >30 || u2FrameRate <5)
	    SENSORDB("error frame rate seting");
	
    if(s5k4e1ga.sensorMode == SENSOR_MODE_PREVIEW)
    {
    	if(s5k4e1ga.S5K4E1GAAutoFlickerMode == KAL_TRUE)
    	{
    		if (u2FrameRate==30||u2FrameRate==24)
				frameRate= 296;
			else
				frameRate= 148;//148;
			
			MAX_Frame_length = (s5k4e1ga.pvPclk*100000)/(S5K4E1GA_PV_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)/frameRate*10;
    	}
		else
		{
			MAX_Frame_length = (s5k4e1ga.pvPclk*100000) /(S5K4E1GA_PV_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels)/u2FrameRate;
		}
    		
		if((MAX_Frame_length <=S5K4E1GA_PV_PERIOD_LINE_NUMS))
		{
			MAX_Frame_length = S5K4E1GA_PV_PERIOD_LINE_NUMS;

		}
		spin_lock(&s5k4e1gamipiraw_drv_lock);
		s5k4e1ga.DummyLines = MAX_Frame_length - S5K4E1GA_PV_PERIOD_LINE_NUMS;
		spin_unlock(&s5k4e1gamipiraw_drv_lock);
		
		S5K4E1GA_SetDummy(s5k4e1ga.DummyPixels,s5k4e1ga.DummyLines);
    }
	
    return KAL_TRUE;
}

UINT32 S5K4E1GASetAutoFlickerMode(kal_bool bEnable, UINT16 u2FrameRate)
{
	SENSORDB("bEnable=%d,u2FrameRate=%d\n", bEnable, u2FrameRate);
	if(bEnable) 
	{   
		spin_lock(&s5k4e1gamipiraw_drv_lock);
		s5k4e1ga.S5K4E1GAAutoFlickerMode = KAL_TRUE;  
		spin_unlock(&s5k4e1gamipiraw_drv_lock);
    } 
	else 
	{
    	spin_lock(&s5k4e1gamipiraw_drv_lock);
        s5k4e1ga.S5K4E1GAAutoFlickerMode = KAL_FALSE; 
		spin_unlock(&s5k4e1gamipiraw_drv_lock);
    }

    return TRUE;
}


UINT32 S5K4E1GAFeatureControl(MSDK_SENSOR_FEATURE_ENUM FeatureId,
                                                                UINT8 *pFeaturePara,UINT32 *pFeatureParaLen)
{
    UINT16 *pFeatureReturnPara16=(UINT16 *) pFeaturePara;
    UINT16 *pFeatureData16=(UINT16 *) pFeaturePara;
    UINT32 *pFeatureReturnPara32=(UINT32 *) pFeaturePara;
    UINT32 *pFeatureData32=(UINT32 *) pFeaturePara;
    UINT32 SensorRegNumber;
    UINT32 i;
    PNVRAM_SENSOR_DATA_STRUCT pSensorDefaultData=(PNVRAM_SENSOR_DATA_STRUCT) pFeaturePara;
    MSDK_SENSOR_CONFIG_STRUCT *pSensorConfigData=(MSDK_SENSOR_CONFIG_STRUCT *) pFeaturePara;
    MSDK_SENSOR_REG_INFO_STRUCT *pSensorRegData=(MSDK_SENSOR_REG_INFO_STRUCT *) pFeaturePara;
    MSDK_SENSOR_GROUP_INFO_STRUCT *pSensorGroupInfo=(MSDK_SENSOR_GROUP_INFO_STRUCT *) pFeaturePara;
    MSDK_SENSOR_ITEM_INFO_STRUCT *pSensorItemInfo=(MSDK_SENSOR_ITEM_INFO_STRUCT *) pFeaturePara;
    MSDK_SENSOR_ENG_INFO_STRUCT	*pSensorEngInfo=(MSDK_SENSOR_ENG_INFO_STRUCT *) pFeaturePara;

	SENSORDB("FeatureId=%d\n",FeatureId);
    switch (FeatureId)
    {
        case SENSOR_FEATURE_GET_RESOLUTION:
            *pFeatureReturnPara16++= S5K4E1GA_IMAGE_SENSOR_FULL_WIDTH;;
            *pFeatureReturnPara16= S5K4E1GA_IMAGE_SENSOR_FULL_HEIGHT;
            *pFeatureParaLen=4;
            break;
        case SENSOR_FEATURE_GET_PERIOD:
			#if defined( S5K4E1GA_1M_SIZE_PREVIEW ) 
        			switch(s_CurrentScenarioId)
        			{
						case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
        				case MSDK_SCENARIO_ID_CAMERA_ZSD:
		            		*pFeatureReturnPara16++= S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels;  
		            		*pFeatureReturnPara16= S5K4E1GA_FULL_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines;	
		            		SENSORDB("Sensor period:%d ,%d\n", S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels, S5K4E1GA_FULL_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines); 
		            		*pFeatureParaLen=4;        				
        					break;
        			
        				default:	
		            		*pFeatureReturnPara16++= S5K4E1GA_PV_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels;  
		            		*pFeatureReturnPara16= S5K4E1GA_PV_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines;
		            		SENSORDB("Sensor period:%d ,%d\n", S5K4E1GA_PV_PERIOD_PIXEL_NUMS  + s5k4e1ga.DummyPixels, S5K4E1GA_PV_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines); 
		            		*pFeatureParaLen=4;
	            			break;
          				}
			#else
		            *pFeatureReturnPara16++= S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels;  
		            *pFeatureReturnPara16= S5K4E1GA_FULL_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines;	
		            SENSORDB("Sensor period:%d ,%d\n", S5K4E1GA_FULL_PERIOD_PIXEL_NUMS + s5k4e1ga.DummyPixels, S5K4E1GA_FULL_PERIOD_LINE_NUMS + s5k4e1ga.DummyLines); 
		            *pFeatureParaLen=4;
			#endif
          	break;
        case SENSOR_FEATURE_GET_PIXEL_CLOCK_FREQ:
			#if defined( S5K4E1GA_1M_SIZE_PREVIEW )
        			switch(s_CurrentScenarioId)
        			{
						case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
        				case MSDK_SCENARIO_ID_CAMERA_ZSD:
		            	 	*pFeatureReturnPara32 = s5k4e1ga.capPclk*100000; //81600000; 
		            	 	*pFeatureParaLen=4;		         	
		         			 break;
		         		
		         		default:
		            		*pFeatureReturnPara32 = s5k4e1ga.pvPclk*100000; //81600000; 
		            		*pFeatureParaLen=4;
		            		break;
		        	}			
			#else
				*pFeatureReturnPara32 = 81600000; 
		         *pFeatureParaLen=4;
			#endif
		    break;
        case SENSOR_FEATURE_SET_ESHUTTER:
            S5K4E1GA_SetShutter(*pFeatureData16);
            break;
        case SENSOR_FEATURE_SET_NIGHTMODE:
            S5K4E1GA_NightMode((BOOL) *pFeatureData16);
            break;
        case SENSOR_FEATURE_SET_GAIN:
            S5K4E1GA_SetGain((UINT16) *pFeatureData16);
            break;
        case SENSOR_FEATURE_SET_FLASHLIGHT:
            break;
        case SENSOR_FEATURE_SET_ISP_MASTER_CLOCK_FREQ:
            //S5K4E1GA_isp_master_clock=*pFeatureData32;
            break;
        case SENSOR_FEATURE_SET_REGISTER:
            S5K4E1GA_write_cmos_sensor(pSensorRegData->RegAddr, pSensorRegData->RegData);
            break;
        case SENSOR_FEATURE_GET_REGISTER:
            pSensorRegData->RegData = S5K4E1GA_read_cmos_sensor(pSensorRegData->RegAddr);
            break;
        case SENSOR_FEATURE_SET_CCT_REGISTER:
            SensorRegNumber=FACTORY_END_ADDR;
            for (i=0;i<SensorRegNumber;i++)
            {
            	spin_lock(&s5k4e1gamipiraw_drv_lock);
                S5K4E1GASensorCCT[i].Addr=*pFeatureData32++;
                S5K4E1GASensorCCT[i].Para=*pFeatureData32++;
				spin_unlock(&s5k4e1gamipiraw_drv_lock);
            }
            break;
        case SENSOR_FEATURE_GET_CCT_REGISTER:
            SensorRegNumber=FACTORY_END_ADDR;
            if (*pFeatureParaLen<(SensorRegNumber*sizeof(SENSOR_REG_STRUCT)+4))
                return FALSE;
            *pFeatureData32++=SensorRegNumber;
            for (i=0;i<SensorRegNumber;i++)
            {
            	spin_lock(&s5k4e1gamipiraw_drv_lock);
                *pFeatureData32++=S5K4E1GASensorCCT[i].Addr;
                *pFeatureData32++=S5K4E1GASensorCCT[i].Para;
				spin_unlock(&s5k4e1gamipiraw_drv_lock);
            }
            break;
        case SENSOR_FEATURE_SET_ENG_REGISTER:
            SensorRegNumber=ENGINEER_END;
            for (i=0;i<SensorRegNumber;i++)
            {
            	spin_lock(&s5k4e1gamipiraw_drv_lock);
                S5K4E1GASensorReg[i].Addr=*pFeatureData32++;
                S5K4E1GASensorReg[i].Para=*pFeatureData32++;
				spin_unlock(&s5k4e1gamipiraw_drv_lock);
            }
            break;
        case SENSOR_FEATURE_GET_ENG_REGISTER:
            SensorRegNumber=ENGINEER_END;
            if (*pFeatureParaLen<(SensorRegNumber*sizeof(SENSOR_REG_STRUCT)+4))
                return FALSE;
            *pFeatureData32++=SensorRegNumber;
            for (i=0;i<SensorRegNumber;i++)
            {
                *pFeatureData32++=S5K4E1GASensorReg[i].Addr;
                *pFeatureData32++=S5K4E1GASensorReg[i].Para;
            }
            break;
        case SENSOR_FEATURE_GET_REGISTER_DEFAULT:
            if (*pFeatureParaLen>=sizeof(NVRAM_SENSOR_DATA_STRUCT))
            {
                pSensorDefaultData->Version=NVRAM_CAMERA_SENSOR_FILE_VERSION;
                pSensorDefaultData->SensorId=S5K4E1GA_SENSOR_ID;
                memcpy(pSensorDefaultData->SensorEngReg, S5K4E1GASensorReg, sizeof(SENSOR_REG_STRUCT)*ENGINEER_END);
                memcpy(pSensorDefaultData->SensorCCTReg, S5K4E1GASensorCCT, sizeof(SENSOR_REG_STRUCT)*FACTORY_END_ADDR);
            }
            else
                return FALSE;
            *pFeatureParaLen=sizeof(NVRAM_SENSOR_DATA_STRUCT);
            break;
        case SENSOR_FEATURE_GET_CONFIG_PARA:
            memcpy(pSensorConfigData, &S5K4E1GASensorConfigData, sizeof(MSDK_SENSOR_CONFIG_STRUCT));
            *pFeatureParaLen=sizeof(MSDK_SENSOR_CONFIG_STRUCT);
            break;
        case SENSOR_FEATURE_CAMERA_PARA_TO_SENSOR:
            S5K4E1GA_camera_para_to_sensor();
            break;

        case SENSOR_FEATURE_SENSOR_TO_CAMERA_PARA:
            S5K4E1GA_sensor_to_camera_para();
            break;
        case SENSOR_FEATURE_GET_GROUP_COUNT:
            *pFeatureReturnPara32++=S5K4E1GA_get_sensor_group_count();
            *pFeatureParaLen=4;
            break;
        case SENSOR_FEATURE_GET_GROUP_INFO:
            S5K4E1GA_get_sensor_group_info(pSensorGroupInfo->GroupIdx, pSensorGroupInfo->GroupNamePtr, &pSensorGroupInfo->ItemCount);
            *pFeatureParaLen=sizeof(MSDK_SENSOR_GROUP_INFO_STRUCT);
            break;
        case SENSOR_FEATURE_GET_ITEM_INFO:
            S5K4E1GA_get_sensor_item_info(pSensorItemInfo->GroupIdx,pSensorItemInfo->ItemIdx, pSensorItemInfo);
            *pFeatureParaLen=sizeof(MSDK_SENSOR_ITEM_INFO_STRUCT);
            break;

        case SENSOR_FEATURE_SET_ITEM_INFO:
            S5K4E1GA_set_sensor_item_info(pSensorItemInfo->GroupIdx, pSensorItemInfo->ItemIdx, pSensorItemInfo->ItemValue);
            *pFeatureParaLen=sizeof(MSDK_SENSOR_ITEM_INFO_STRUCT);
            break;

        case SENSOR_FEATURE_GET_ENG_INFO:
            pSensorEngInfo->SensorId = 129;
            pSensorEngInfo->SensorType = CMOS_SENSOR;
            pSensorEngInfo->SensorOutputDataFormat=SENSOR_OUTPUT_FORMAT_RAW_B;
            *pFeatureParaLen=sizeof(MSDK_SENSOR_ENG_INFO_STRUCT);
            break;
        case SENSOR_FEATURE_GET_LENS_DRIVER_ID:
            // get the lens driver ID from EEPROM or just return LENS_DRIVER_ID_DO_NOT_CARE
            // if EEPROM does not exist in camera module.
            *pFeatureReturnPara32=LENS_DRIVER_ID_DO_NOT_CARE;
            *pFeatureParaLen=4;
            break;

        case SENSOR_FEATURE_INITIALIZE_AF:
            break;
        case SENSOR_FEATURE_CONSTANT_AF:
            break;
        case SENSOR_FEATURE_MOVE_FOCUS_LENS:
            break;
        case SENSOR_FEATURE_SET_VIDEO_MODE:
            S5K4E1GASetVideoMode(*pFeatureData16);
            break;
        case SENSOR_FEATURE_CHECK_SENSOR_ID:
            S5K4E1GAGetSensorID(pFeatureReturnPara32); 
            break;             
        case SENSOR_FEATURE_SET_AUTO_FLICKER_MODE:
            S5K4E1GASetAutoFlickerMode((BOOL)*pFeatureData16, *(pFeatureData16+1));            
	        break;
        case SENSOR_FEATURE_SET_TEST_PATTERN:
            S5K4E1GASetTestPatternMode((BOOL)*pFeatureData16);        	
            break;
        case SENSOR_FEATURE_GET_TEST_PATTERN_CHECKSUM_VALUE://for factory mode auto testing             
            *pFeatureReturnPara32= S5K4E1GA_TEST_PATTERN_CHECKSUM;
            *pFeatureParaLen=4;                             
            break;
		case SENSOR_FEATURE_SET_MAX_FRAME_RATE_BY_SCENARIO:
			S5K4E1SetMaxFramerateByScenario((MSDK_SCENARIO_ID_ENUM)*pFeatureData32, *(pFeatureData32+1));
		break;
		case SENSOR_FEATURE_GET_DEFAULT_FRAME_RATE_BY_SCENARIO:
			S5K4E1GetDefaultFramerateByScenario((MSDK_SCENARIO_ID_ENUM)*pFeatureData32, (MUINT32 *)(*(pFeatureData32+1)));
		break;
        default:
            break;
    }
    return ERROR_NONE;
}	/* S5K4E1GAFeatureControl() */


SENSOR_FUNCTION_STRUCT	SensorFuncS5K4E1GA=
{
    S5K4E1GAOpen,
    S5K4E1GAGetInfo,
    S5K4E1GAGetResolution,
    S5K4E1GAFeatureControl,
    S5K4E1GAControl,
    S5K4E1GAClose
};

UINT32 S5K4E1GA_MIPI_RAW_SensorInit(PSENSOR_FUNCTION_STRUCT *pfFunc)
{
    /* To Do : Check Sensor status here */
    if (pfFunc!=NULL)
        *pfFunc=&SensorFuncS5K4E1GA;

    return ERROR_NONE;
}   /* SensorInit() */

