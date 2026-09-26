#include "main.h"

void HAL_MspInit(void)
{
	__HAL_RCC_AFIO_CLK_ENABLE();
	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_AFIO_REMAP_SWJ_NOJTAG();
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
	GPIO_InitTypeDef gpio = {0};

	if (huart->Instance == USART1) {
		__HAL_RCC_USART1_CLK_ENABLE();
		__HAL_RCC_GPIOA_CLK_ENABLE();

		/* PA9 = USART1_TX (test point TP6) */
		gpio.Pin = GPIO_PIN_9;
		gpio.Mode = GPIO_MODE_AF_PP;
		gpio.Speed = GPIO_SPEED_FREQ_HIGH;
		HAL_GPIO_Init(GPIOA, &gpio);

		/* PA10 = USART1_RX (test point TP5) */
		gpio.Pin = GPIO_PIN_10;
		gpio.Mode = GPIO_MODE_INPUT;
		gpio.Pull = GPIO_PULLUP;
		HAL_GPIO_Init(GPIOA, &gpio);
	}
}
